#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"

#include <libretro.h>

#include "gles.h"
#include "rend/grow_buf.h"
#include "gl_texdecoder.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/texdecode.h"

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
/* (the pool's formats: desktop OpenGL's, which the OpenGL ES 2 headers have not) */
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
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

#ifndef GL_IMPLEMENTATION_COLOR_READ_TYPE
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#endif

#ifndef GL_IMPLEMENTATION_COLOR_READ_FORMAT
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#endif

/*
Textures

Textures are converted to native OpenGL textures
The mapping is done with tcw:tsp -> GL texture. That includes stuff like
filtering/ texture repeat

To save space native formats are used for 1555/565/4444 (only bit shuffling is done)
YUV is converted to 8888
PALs are decoded to their unpaletted format (5551/565/4444/8888 depending on palette type)

Compression
	look into it, but afaik PVRC is not realtime doable
*/

/* The cache's textures (rend/TexCache.h), by their key */
static tex_map_t tex_cache;
static const texcache_ops gl_texture_ops;

static void tex_cache_ready(void)
{
	if (tex_cache.elem == 0)
	{
		tex_map_init(&tex_cache, sizeof(gl_texture), NULL, NULL);
		tex_cache.name = "Texture cache";
	}
}

void gl_texcache_clear(void)
{
	tex_cache_ready();
	texcache_map_clear(&tex_cache);
}

void gl_texcache_collect_cleanup(void)
{
	tex_cache_ready();
	texcache_map_collect(&tex_cache);
}

static gl_texture *gl_texture_of(u32 tsp, u32 tcw, int area)
{
	tex_cache_ready();
	return (gl_texture *)texcache_map_get(&tex_cache, &gl_texture_ops, tsp, tcw, area);
}

/* ---- The texture pool ---- */

/* By TextureType, and the pictures rendered to: the internal format, and
 * the format and type of what is uploaded */
static const GLenum poolInternal[GL_POOL_FORMATS] = { GL_RGB565, GL_RGB5_A1, GL_RGBA4, GL_RGBA8, GL_R8, GL_RGBA8, GL_RGB8 };
static const GLenum poolFormat[GL_POOL_FORMATS] = { GL_RGB, GL_RGBA, GL_RGBA, GL_RGBA, GL_RED, GL_RGBA, GL_RGB };
static const GLenum poolType[GL_POOL_FORMATS] = {
	GL_UNSIGNED_SHORT_5_6_5, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE
};
static const u32 poolTexel[GL_POOL_FORMATS] = { 2, 2, 2, 4, 1, 4, 4 };
static const char * const poolNames[GL_POOL_FORMATS] = { "565", "5551", "4444", "8888", "8-bit", "RGBA8 rendered to", "RGB8 rendered to" };

static struct
{
	tex_pool_book_t book;
	bool ready;
	bool storage;		/* glTexStorage2D() */
	GLPooledTexture *all;	/* every one there is, for gl_tex_pool_term() */
} pool;

/* One of class @cls made: at the start, or (@late) because the class had
 * none left */
static GLPooledTexture *pool_make(int cls, bool late)
{
	const u32 f = tex_pool_format(cls);
	const u32 width = tex_pool_width(cls), height = tex_pool_height(cls);
	GLPooledTexture *t = (GLPooledTexture *)calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->levels = tex_pool_mips(cls) ? 1 + bitscanrev(width > height ? width : height) : 1;
	glGenTextures(1, &t->name);
	glc_bind_texture(GL_TEXTURE_2D, t->name);
#ifndef HAVE_OPENGLES2
	if (pool.storage)
		glTexStorage2D(GL_TEXTURE_2D, t->levels, poolInternal[f], width, height);
	else
#endif
	{
		u32 l;
		for (l = 0; l < t->levels; l++)
			glTexImage2D(GL_TEXTURE_2D, l, poolInternal[f], (width >> l) > 1 ? width >> l : 1, (height >> l) > 1 ? height >> l : 1,
					0, poolFormat[f], poolType[f], NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
	}
	if (glGetError() != GL_NO_ERROR)
	{
		WARN_LOG(RENDERER, "OpenGL: texture pool: a %ux%u %s texture could not be made\n", width, height, poolNames[f]);
		glc_delete_textures(1, &t->name);
		free(t);
		return NULL;
	}
	tex_pool_made(&pool.book, &t->entry, cls, tex_pool_bytes(cls, poolTexel[f]), late, poolNames[f]);
	t->all = pool.all;
	pool.all = t;
	return t;
}

static void gl_tex_pool_init(void)
{
	/* how much games use them, in eighths of what they use 565 */
	static const u32 use[GL_POOL_FORMATS] = { 8, 6, 6, 4, 2, 0, 0 };
	static const u8 kind[GL_POOL_FORMATS] = { 0, 0, 0, 0, 0, TEX_POOL_TARGETS, TEX_POOL_TARGETS };
	/* (the renderer's thread's, at its start) */
	static u32 counts[TEX_POOL_CLASSES];
	VramBudget budget;
	u64 bytes;
	int c;

	if (pool.ready || gl.gl_major < 3)
		return;
	pool.storage = gl.is_gles || gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 2);
	tex_pool_reset(&pool.book, 1);
	/* (A driver that does not say what video memory there is - neither
	 * GL_NVX_gpu_memory_info nor GL_ATI_meminfo, which is most of them - has
	 * a budget that is a guess, and a low one: the pool is made as for a
	 * card with 1.5 GB to spare, 96 MB of textures, which the busiest
	 * games' scenes need.) */
	budget = gl.vram_budget;
	if (!budget.exact && vram_budget_available_kib(&budget) < (1536u << 10))
	{
		budget.budget_kib = 1536u << 10;
		budget.usage_kib = 0;
	}
	bytes = tex_pool_plan(&budget, poolTexel, use, kind, GL_POOL_FORMATS, counts,
			texcache_upscale_factor(), (u32)settings.rend.MaxFilteredTextureSize,
			settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1u);
	pool.ready = true;
	while (glGetError() != GL_NO_ERROR)
		;
	for (c = 0; c < TEX_POOL_CLASSES; c++)
	{
		u32 i;
		for (i = 0; i < counts[c]; i++)
		{
			GLPooledTexture *t = pool_make(c, false);
			if (t == NULL)
				break;
			tex_pool_give(&pool.book, &t->entry);
		}
	}
	NOTICE_LOG(RENDERER, "OpenGL: texture pool of %u textures, %u MB (planned %u MB)\n", pool.book.made,
			(u32)(pool.book.bytesMade >> 20), (u32)(bytes >> 20));
}

static void gl_tex_pool_term(void)
{
	if (!pool.ready)
		return;
	tex_pool_summary(&pool.book, "OpenGL");
	while (pool.all != NULL)
	{
		GLPooledTexture *next = pool.all->all;
		glc_delete_textures(1, &pool.all->name);
		free(pool.all);
		pool.all = next;
	}
	tex_pool_reset(&pool.book, 1);
	pool.ready = false;
}

bool gl_tex_pool_ready(void)
{
	return pool.ready;
}

static GLPooledTexture *pool_take_class(int cls)
{
	TexPoolEntry *e;
	if (!pool.ready || cls < 0)
		return NULL;
	e = tex_pool_take(&pool.book, cls);
	/* (the entry first: the texture's) */
	return e != NULL ? (GLPooledTexture *)e : pool_make(cls, true);
}

GLPooledTexture *gl_tex_pool_take(int type, u32 width, u32 height, bool mips)
{
	if ((unsigned)type >= 5)
		return NULL;
	return pool_take_class(tex_pool_class((u32)type, width, height, mips));
}

GLPooledTexture *gl_tex_pool_take_target(bool rgb, u32 width, u32 height)
{
	return pool_take_class(tex_pool_class(rgb ? GL_POOL_TARGET_RGB : GL_POOL_TARGET_RGBA, width, height, false));
}

void gl_tex_pool_give(GLPooledTexture *t)
{
	tex_pool_give(&pool.book, &t->entry);
}

static GLPooledTexture *pool_find(GLuint name)
{
	GLPooledTexture *t;
	for (t = pool.all; t != NULL; t = t->all)
		if (t->name == name)
			return t;
	return NULL;
}

static void rtt_gl_init(void);
static void upload_ring_init(void);
static void upload_ring_term(void);
static void upload_ring_fence(void);
static void upload_ring_follow(void);

void gl_tex_init(bool depth)
{
	gltd_init();
	gl_tex_pool_init();
	upload_ring_init();
	/* what a render read back takes, made now (rtt_gl_init()) */
	rtt_gl_init();
	/* ...and the cache's room for them (rend/tex_map.h) */
	tex_cache_ready();
	texcache_map_reserve_for(&tex_cache, pool.book.made);
	gl.rtt.kept = false;
	gl.rtt.pooled = NULL;
	if (!pool.ready)
		return;
	/* The framebuffer renders to textures are drawn through, and their
	 * depth buffer, now and for as long as the renderer is: as large as
	 * the largest texture of the pool's, at the scale they are made at */
	glGenFramebuffers(1, &gl.rtt.fbo);
	gl.rtt.depthWidth = gl.rtt.depthHeight = 0;
	if (depth)
	{
		u32 side;
		u32 room;
		u32 scale;
		GLint most = 0;
		glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &most);
		scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
		room = (u32)(most > 1024 ? most : 1024);
		side = 1024 * scale < room ? 1024 * scale : room;
		glGenRenderbuffers(1, &gl.rtt.depthb);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, gl.depth_format, side, side);
		gl.rtt.depthWidth = gl.rtt.depthHeight = side;
	}
	gl.rtt.kept = true;
}

static void rtt_gl_term(void);
static void fb_texture_term(void);

void gl_tex_term(void)
{
	if (gl.rtt.kept)
	{
		if (gl.rtt.fbo)
			glDeleteFramebuffers(1, &gl.rtt.fbo);
		if (gl.rtt.depthb)
			glDeleteRenderbuffers(1, &gl.rtt.depthb);
		gl.rtt.fbo = gl.rtt.depthb = 0;
		gl.rtt.kept = false;
	}
	if (gl.rtt.pooled != NULL)
	{
		gl_tex_pool_give(gl.rtt.pooled);
		gl.rtt.pooled = NULL;
		gl.rtt.tex = 0;
	}
	upload_ring_term();
	gl_tex_pool_term();
	gltd_term();
	rtt_gl_term();
	fb_texture_term();
}

void gl_tex_flush(void)
{
	gltd_flush();
	upload_ring_fence();
	upload_ring_follow();
}

/* ---- Where textures converted on the CPU wait ----
 *
 * Where the textures are not made on the GPU (OpenGL ES, OpenGL before
 * 4.3), a texture of the pool's is converted straight into a piece of one
 * pixel unpack buffer, made when the renderer starts, and taken from there
 * (TextureCacheData::UploadMemory()): no memory of the cache's grown for
 * it, and no copy of it out of that. The pieces are handed out one after
 * the other, a ring, and a frame's are taken back when its fence, polled,
 * says the GPU is done with them. Nothing waits: with the ring held by
 * frames still on the GPU there is no piece, and the texture is converted
 * into the cache's memory as before. */
#if !defined(HAVE_OPENGLES2) && defined(GL_PIXEL_UNPACK_BUFFER) && defined(GL_SYNC_GPU_COMMANDS_COMPLETE)
#ifndef GL_MAP_INVALIDATE_RANGE_BIT
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#define UPLOAD_SPANS 16

static struct
{
	GLuint buffer;
	size_t size, head, open;
	u32 first, count;
	struct { size_t start, end; GLsync fence; } span[UPLOAD_SPANS];
	u8 *mapped;
	size_t at, len;
	bool saidFull;
	bool tried;	/* made, or tried to be, since the textures were made on the GPU */
} upload;

/* Whether textures are converted on the CPU: no decoder, or the GPU's
 * decoding turned off (GPU Texture Decoding, which takes effect with the
 * next texture) */
static bool upload_ring_wanted(void)
{
	return pool.ready && (!gltd_ready() || !settings.rend.GpuTextureDecode)
			&& (gl.is_gles || gl.gl_major > 3 || (gl.gl_major == 3 && gl.gl_minor >= 2));
}

static void upload_ring_init(void)
{
	u64 bytes;
	upload_ring_term();
	upload.tried = upload_ring_wanted();
	if (!upload.tried)
		return;
	/* A share of the video memory: 8 to 16 MB, room for the largest
	 * texture, 1024x1024 with its mipmaps at 32 bits, and more */
	bytes = (u64)vram_budget_available_kib(&gl.vram_budget) * 1024 / 128;
	if (bytes < (8 << 20))
		bytes = 8 << 20;
	if (bytes > (16 << 20))
		bytes = 16 << 20;
	while (glGetError() != GL_NO_ERROR)
		;
	glGenBuffers(1, &upload.buffer);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, upload.buffer);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	if (glGetError() != GL_NO_ERROR)
	{
		glDeleteBuffers(1, &upload.buffer);
		upload.buffer = 0;
		return;
	}
	upload.size = (size_t)bytes;
	NOTICE_LOG(RENDERER, "OpenGL: textures converted on the CPU go through a ring of %u KB\n", (u32)(bytes >> 10));
}

/* The ring made when the GPU's decoding is turned off while the game runs
 * (after the frame: as a change of resolution makes what it needs), and
 * kept if it is turned on again */
static void upload_ring_follow(void)
{
	if (upload.buffer == 0 && !upload.tried && upload_ring_wanted())
		upload_ring_init();
	else if (upload.tried && !upload_ring_wanted() && upload.buffer == 0)
		upload.tried = false;
}

static void upload_unmap(void)
{
	if (upload.mapped == NULL)
		return;
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, upload.buffer);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	upload.mapped = NULL;
}

static void upload_ring_term(void)
{
	u32 i;
	if (upload.buffer == 0)
		return;
	upload_unmap();
	for (i = 0; i < upload.count; i++)
		glDeleteSync(upload.span[(upload.first + i) % UPLOAD_SPANS].fence);
	glDeleteBuffers(1, &upload.buffer);
	memset(&upload, 0, sizeof(upload));
}

/* The frame's pieces, to be taken back once the GPU has done with them */
static void upload_ring_fence(void)
{
	u32 i;
	if (upload.buffer == 0 || upload.head == upload.open || upload.count == UPLOAD_SPANS)
		return;
	i = (upload.first + upload.count) % UPLOAD_SPANS;
	upload.span[i].start = upload.open;
	upload.span[i].end = upload.head;
	upload.span[i].fence = (GLsync)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (upload.span[i].fence == 0)
		return;
	upload.count++;
	upload.open = upload.head;
}

static u8 *upload_ring_take(size_t len)
{
	size_t live, at;
	if (upload.buffer == 0)
		return NULL;
	upload_unmap();
	while (upload.count != 0)
	{
		GLsync f = upload.span[upload.first].fence;
		GLenum r = glClientWaitSync(f, 0, 0);
		if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
			break;
		glDeleteSync(f);
		upload.first = (upload.first + 1) % UPLOAD_SPANS;
		upload.count--;
	}
	len = (len + 15) & ~(size_t)15;
	live = upload.count != 0 ? upload.span[upload.first].start : upload.open;
	if (live == upload.head)
		upload.head = upload.open = live = 0;
	if (upload.head >= live)
	{
		/* free: what is after the head, then what is before the oldest */
		if (upload.head + len <= upload.size)
			at = upload.head;
		else if (len < live)
			at = 0;
		else
			at = upload.size;
	}
	else
		at = upload.head + len < live ? upload.head : upload.size;
	if (at == upload.size)
	{
		if (!upload.saidFull)
			INFO_LOG(RENDERER, "OpenGL: the ring textures go through is full: one converted in memory of the cache's\n");
		upload.saidFull = true;
		return NULL;
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, upload.buffer);
	upload.mapped = (u8 *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, (GLintptr)at, (GLsizeiptr)len,
			GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	if (upload.mapped == NULL)
		return NULL;
	upload.at = at;
	upload.len = len;
	upload.head = at + len;
	return upload.mapped;
}

/* What the upload reads, if it is in the piece mapped: the piece let go of,
 * the buffer bound, and the offset in it */
static u8 *upload_ring_source(u8 *data, bool *bound)
{
	size_t offset;
	*bound = false;
	if (upload.mapped == NULL || data < upload.mapped || data >= upload.mapped + upload.len)
		return data;
	offset = upload.at + (size_t)(data - upload.mapped);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, upload.buffer);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	upload.mapped = NULL;
	*bound = true;
	return (u8 *)(uintptr_t)offset;
}

static void upload_ring_done(bool bound)
{
	if (bound)
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}
#else
static void upload_ring_init(void) { }
static void upload_ring_term(void) { }
static void upload_ring_fence(void) { }
static u8 *upload_ring_take(size_t len) { return NULL; }
static void upload_ring_follow(void) { }
static u8 *upload_ring_source(u8 *data, bool *bound) { *bound = false; return data; }
static void upload_ring_done(bool bound) { }
#endif

static bool tex_take_pooled(gl_texture *t, u32 width, u32 height, bool mips)
{
	GLPooledTexture *p;
	if (!pool.ready || t->texID == 0)
		return false;
	if (t->pooled != NULL && (int)t->pooled->entry.cls == tex_pool_class((u32)t->base.tex_type, width, height, mips))
		return true;
	p = gl_tex_pool_take(t->base.tex_type, width, height, mips);
	if (p == NULL)
		return false;
	if (t->pooled != NULL)
		gl_tex_pool_give(t->pooled);
	else
		t->ownID = t->texID;
	t->pooled = p;
	t->texID = p->name;
	return true;
}

static void tex_release_pooled(gl_texture *t)
{
	if (t->pooled == NULL)
		return;
	gl_tex_pool_give(t->pooled);
	t->pooled = NULL;
	t->texID = t->ownID != 0 ? t->ownID : glc_gen_texture();
	t->ownID = 0;
}

static void tex_cancel_decode(gl_texture *t)
{
	if (t->decodeSerial != 0)
		gltd_cancel(t->decodeJob, t->decodeSerial);
	t->decodeSerial = 0;
}

static void *tex_upload_memory(texcache_data *base, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	gl_texture *t = (gl_texture *)base;
	(void)pixel;
	if (t->texID == 0 || !tex_take_pooled(t, width, height, mipmaps))
		return NULL;
	return upload_ring_take(bytes);
}

/* Made on the GPU (gl_texdecoder.h), or copied as it is. Everything that
 * can say no does before anything is changed but the texture it is made
 * into, which a no leaves for the CPU's upload to take. */
static bool tex_gpu_decode(texcache_data *base, const struct texd_plan *plan)
{
	gl_texture *t = (gl_texture *)base;
	const int type = base->tex_type;
	u32 offset;
	u8 *to;

	if (t->texID == 0)
		return false;
	tex_cancel_decode(t);
	if (plan->native)
	{
		const u32 stride = plan->level[0].sr & 0xFFFF;
		GLenum format = GL_RGB, gltype = GL_UNSIGNED_SHORT_5_6_5, internal = GL_RGB;
		if ((unsigned)type >= 3)
			return false;
		if (gltd_native(type))
		{
			/* Through the ring, into the pool's texture: the PowerVR's
			 * texels are OpenGL's 5-6-5, 1-5-5-5 reversed and 4-4-4-4
			 * reversed as they are */
			if (!tex_take_pooled(t, plan->width, plan->height, false))
				return false;
			to = gltd_begin(plan->bytes, 0, &offset);
			if (to == NULL)
				return false;
			memcpy(to, &vram.data[plan->src], plan->bytes);
			t->decodeSerial = gltd_serial();
			t->decodeJob = gltd_add_copy(t->texID, type, offset + plan->level[0].data, stride, plan->width, plan->height);
			return true;
		}
		if (pool.ready)
			return false;
		/* Straight from video memory: OpenGL ES has 5-6-5 alone, and no row
		 * length but the texture's */
		if (gl.is_gles && (type != TEX_TYPE_565 || stride != plan->width))
			return false;
		if (type == TEX_TYPE_5551)
		{
			format = GL_BGRA;
			gltype = GL_UNSIGNED_SHORT_1_5_5_5_REV;
			internal = GL_RGBA;
		}
		else if (type == TEX_TYPE_4444)
		{
			format = GL_BGRA;
			gltype = GL_UNSIGNED_SHORT_4_4_4_4_REV;
			internal = GL_RGBA;
		}
		glc_bind_texture(GL_TEXTURE_2D, t->texID);
		if (stride != plan->width)
			glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
		glTexImage2D(GL_TEXTURE_2D, 0, internal, plan->width, plan->height, 0, format, gltype, &vram.data[plan->src + plan->level[0].data]);
		if (stride != plan->width)
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		return true;
	}
	if (!gltd_takes(type, plan->out) || !tex_take_pooled(t, plan->width, plan->height, plan->mipmapped != 0)
			|| t->pooled->levels < plan->levels)
		return false;
	to = gltd_begin(plan->pal + plan->pal_entries * 4, plan->levels, &offset);
	if (to == NULL)
		return false;
	memcpy(to, &vram.data[plan->src], plan->bytes);
	if (plan->pal_entries != 0)
		memcpy(to + plan->pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);
	t->decodeSerial = gltd_serial();
	t->decodeJob = gltd_add_decode(t->texID, type, plan, offset);
	return true;
}

/* A texture of its own to be made anew: a custom texture's */
static void tex_renew(gl_texture *t)
{
	tex_cancel_decode(t);
	if (t->pooled != NULL)
		tex_release_pooled(t);
	else
	{
		glc_delete_textures(1, &t->texID);
		t->texID = glc_gen_texture();
	}
}

/* The picture rendered to is the texture now: @tex, the pool's @pooled or
 * one of its own */
static void tex_set_rendered(gl_texture *t, GLuint tex, GLPooledTexture *pooled)
{
	tex_cancel_decode(t);
	if (t->pooled != NULL)
	{
		gl_tex_pool_give(t->pooled);
		t->pooled = NULL;
		if (t->ownID != 0)
			glc_delete_textures(1, &t->ownID);
		t->ownID = 0;
	}
	else if (t->texID != 0)
		glc_delete_textures(1, &t->texID);
	t->pooled = pooled;
	t->texID = tex;
}

static void tex_upload_from(gl_texture *t, int width, int height, u8 *temp_tex_buffer, bool mipmapped, bool mipmapsIncluded)
{
	const int type = t->base.tex_type;
	GLuint comps, gltype;
	u32 bytes_per_pixel = 2;

	if (t->texID != 0 && tex_take_pooled(t, width, height, mipmapped || mipmapsIncluded))
	{
		/* Into the pool's texture, made before the game with every level
		 * it can have */
		const u32 f = (u32)type;
		const u32 bpp = poolTexel[f];
		glc_bind_texture(GL_TEXTURE_2D, t->texID);
		if (mipmapsIncluded)
		{
			int i;
			for (i = 0; (1 << i) <= width; i++)
			{
				glTexSubImage2D(GL_TEXTURE_2D, t->pooled->levels - i - 1, 0, 0, 1 << i, 1 << i, poolFormat[f], poolType[f], temp_tex_buffer);
				temp_tex_buffer += (1 << (2 * i)) * bpp;
			}
		}
		else
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, poolFormat[f], poolType[f], temp_tex_buffer);
			if (mipmapped)
				glGenerateMipmap(GL_TEXTURE_2D);
		}
		return;
	}
	tex_release_pooled(t);
	if (t->texID == 0)
	{
		die("Soft rend disabled, invalid code path");
		return;
	}
	/* upload to OpenGL */
	glc_bind_texture(GL_TEXTURE_2D, t->texID);
	comps = type == TEX_TYPE_8 ? gl.single_channel_format : GL_RGBA;
	switch (type)
	{
	case TEX_TYPE_5551:
		gltype = GL_UNSIGNED_SHORT_5_5_5_1;
		break;
	case TEX_TYPE_565:
		gltype = GL_UNSIGNED_SHORT_5_6_5;
		comps = GL_RGB;
		break;
	case TEX_TYPE_4444:
		gltype = GL_UNSIGNED_SHORT_4_4_4_4;
		break;
	case TEX_TYPE_8888:
		bytes_per_pixel = 4;
		gltype = GL_UNSIGNED_BYTE;
		break;
	case TEX_TYPE_8:
		bytes_per_pixel = 1;
		gltype = GL_UNSIGNED_BYTE;
		break;
	default:
		die("Unsupported texture type");
		gltype = 0;
		break;
	}
	if (mipmapsIncluded)
	{
		int mipmapLevels = 0;
		int dim = width;
		int i;
		while (dim != 0)
		{
			mipmapLevels++;
			dim >>= 1;
		}
#if !defined(HAVE_OPENGLES2) && !defined(__APPLE__)
		/* OpenGL 4.2 or OpenGL ES 3.0 at least */
		if (gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 2)
				|| (gl.is_gles && gl.gl_major >= 3))
		{
			GLuint internalFormat;
			switch (type)
			{
			case TEX_TYPE_5551:
				internalFormat = GL_RGB5_A1;
				break;
			case TEX_TYPE_565:
				internalFormat = GL_RGB565;
				break;
			case TEX_TYPE_4444:
				internalFormat = GL_RGBA4;
				break;
			case TEX_TYPE_8888:
				internalFormat = GL_RGBA8;
				break;
			case TEX_TYPE_8:
				internalFormat = gl.single_channel_internal;
				break;
			default:
				die("Unsupported texture format");
				internalFormat = 0;
				break;
			}
			if (t->base.Updates == 1)
			{
				glTexStorage2D(GL_TEXTURE_2D, mipmapLevels, internalFormat, width, height);
				glCheck();
			}
			for (i = 0; i < mipmapLevels; i++)
			{
				glTexSubImage2D(GL_TEXTURE_2D, mipmapLevels - i - 1, 0, 0, 1 << i, 1 << i, comps, gltype, temp_tex_buffer);
				temp_tex_buffer += (1 << (2 * i)) * bytes_per_pixel;
			}
		}
		else
#endif
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipmapLevels - 1);
			for (i = 0; i < mipmapLevels; i++)
			{
				glTexImage2D(GL_TEXTURE_2D, mipmapLevels - i - 1, type == TEX_TYPE_8 ? gl.single_channel_internal : comps, 1 << i, 1 << i, 0, comps, gltype, temp_tex_buffer);
				temp_tex_buffer += (1 << (2 * i)) * bytes_per_pixel;
			}
		}
	}
	else
	{
		glTexImage2D(GL_TEXTURE_2D, 0, type == TEX_TYPE_8 ? gl.single_channel_internal : comps, width, height, 0, comps, gltype, temp_tex_buffer);
		if (mipmapped)
			glGenerateMipmap(GL_TEXTURE_2D);
	}
	glCheck();
}

static void tex_upload_to_gpu(texcache_data *base, int width, int height, u8 *temp_tex_buffer, bool mipmapped, bool mipmapsIncluded)
{
	gl_texture *t = (gl_texture *)base;
	bool bound;
	tex_cancel_decode(t);
	temp_tex_buffer = upload_ring_source(temp_tex_buffer, &bound);
	tex_upload_from(t, width, height, temp_tex_buffer, mipmapped, mipmapsIncluded);
	upload_ring_done(bound);
}

/* The texture the last whole update made, its shape the pool's if it is
 * the pool's, and nothing the decoder still has to do to it */
static bool tex_part_memory(texcache_data *base, size_t bytes, void **memory)
{
	gl_texture *t = (gl_texture *)base;
	*memory = NULL;
	if (base->made_type != (u8)base->tex_type + 1 || t->texID == 0 || (t->decodeSerial != 0 && t->decodeSerial == gltd_serial()))
		return false;
	if (t->pooled == NULL)
		return true;
	if ((int)t->pooled->entry.cls != tex_pool_class((u32)base->tex_type, base->w, base->h, base->made_mips))
		return false;
	/* (through the ring, where there is one and room in it) */
	*memory = upload_ring_take(bytes);
	return true;
}

static void tex_upload_part(texcache_data *base, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	gl_texture *t = (gl_texture *)base;
	const int type = base->tex_type;
	const u32 f = (u32)type;
	GLenum format, gltype;
	u32 i;
	bool bound;

	data = upload_ring_source(data, &bound);
	if (t->pooled != NULL)
	{
		format = poolFormat[f];
		gltype = poolType[f];
	}
	else
	{
		/* as tex_upload_from() makes it */
		format = type == TEX_TYPE_8 ? gl.single_channel_format : type == TEX_TYPE_565 ? GL_RGB : GL_RGBA;
		gltype = type == TEX_TYPE_5551 ? GL_UNSIGNED_SHORT_5_5_5_1 : type == TEX_TYPE_565 ? GL_UNSIGNED_SHORT_5_6_5
				: type == TEX_TYPE_4444 ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_BYTE;
	}
	glc_bind_texture(GL_TEXTURE_2D, t->texID);
	for (i = 0; i < count; i++)
		glTexSubImage2D(GL_TEXTURE_2D, regions[i].level, regions[i].x, regions[i].y, regions[i].w, regions[i].h, format, gltype, data + offset[i]);
	upload_ring_done(bound);
	glCheck();
}

/* The decoder's textures are made whole */
static int tex_gpu_decode_part(texcache_data *base, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	(void)regions;
	(void)count;
	return plan->native || gltd_takes(base->tex_type, plan->out) ? TEXP_WHOLE : TEXP_CPU;
}

static bool tex_delete(texcache_data *base)
{
	gl_texture *t = (gl_texture *)base;
	if (!texcache_delete(base))
		return false;
	tex_cancel_decode(t);

	/* (a render to a texture that video memory has not got yet is kept: see rend/rtt_watch.h) */
	if (t->texID && !rtt_watch_take(t->texID))
	{
		if (t->pooled != NULL)
			gl_tex_pool_give(t->pooled);
		else
			glc_delete_textures(1, &t->texID);
	}
	t->pooled = NULL;
	if (t->ownID != 0 && t->ownID != t->texID)
		glc_delete_textures(1, &t->ownID);
	t->ownID = 0;

	return true;
}

static void tex_get_id(texcache_data *base, char *s, size_t n)
{
	(void)n;
	sprintf(s, "%u", (unsigned)((gl_texture *)base)->texID);
}

static bool tex_force_32bit(const texcache_data *t, int type)
{
	(void)t;
	(void)type;
	return false;
}

/* The framebuffers a read attaches the picture to, and the picture of the
 * console's size a finer one is brought down to: made with the renderer
 * for renders of a screen's size (rtt_gl_init()), larger when a larger
 * one is read, which is said */
static GLuint rtt_read_fbo, rtt_small_fbo, rtt_small_rb;
/* (what a render copied into a texture is drawn into: TextureCacheData::CopyRender()) */
static GLuint rtt_copy_fbo;
static u32 rtt_small_w, rtt_small_h;
static bool rtt_said_larger;

#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(GL_RGBA8)
static void rtt_gl_small(u32 w, u32 h)
{
	if (rtt_small_rb != 0 && w <= rtt_small_w && h <= rtt_small_h)
		return;
	if (rtt_small_rb != 0 && !rtt_said_larger)
	{
		rtt_said_larger = true;
		WARN_LOG(RENDERER, "Render to a texture of %u x %u read back: its room made larger\n", w, h);
	}
	if (w > rtt_small_w)
		rtt_small_w = w;
	if (h > rtt_small_h)
		rtt_small_h = h;
	if (rtt_small_rb == 0)
		glGenRenderbuffers(1, &rtt_small_rb);
	glBindRenderbuffer(RARCH_GL_RENDERBUFFER, rtt_small_rb);
	glRenderbufferStorage(RARCH_GL_RENDERBUFFER, GL_RGBA8, rtt_small_w, rtt_small_h);
	if (rtt_small_fbo == 0)
		glGenFramebuffers(1, &rtt_small_fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_small_fbo);
	glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, RARCH_GL_RENDERBUFFER, rtt_small_rb);
}
#endif

/* The framebuffer the picture is read from, bound: @tex's, or - finer than
 * the console's, made its size on the card, filtered - the small one's.
 * Where it is in that, at (*x, *y). False if it cannot be made smaller. */
static bool rtt_gl_source(const RttWatch *watch, u32 *x, u32 *y)
{
	GLboolean was_scissor;
	const u32 w = watch->w, h = watch->h, scale = watch->scale;

	if (rtt_read_fbo == 0)
		glGenFramebuffers(1, &rtt_read_fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_read_fbo);
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)watch->tex, 0);
	*x = watch->x0 * (scale > 1 ? scale : 1);
	*y = watch->y0 * (scale > 1 ? scale : 1);
	if (scale <= 1)
		return true;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(GL_RGBA8)
	if (gl.gl_major < 3)
		return false;
	was_scissor = glIsEnabled(GL_SCISSOR_TEST);

	rtt_gl_small(w, h);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, rtt_read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rtt_small_fbo);
	if (was_scissor)
		glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(*x, *y, *x + w * scale, *y + h * scale, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	if (was_scissor)
		glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_small_fbo);
	*x = *y = 0;
	return true;
#else
	return false;
#endif
}

/* The picture's framebuffer let go of the texture again: a texture attached
 * to a framebuffer is not deleted when it is (rtt_gl_release()) */
static void rtt_gl_detach(void)
{
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_read_fbo);
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

/* From the texture rendered to, through the framebuffer it is read through,
 * a blit (OpenGL 3, OpenGL ES 3: glsm has no copy into a texture); the rest
 * of each row a narrower stride leaves, cleared. What it changes it puts
 * back. */
static bool tex_copy_render(texcache_data *base, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	gl_texture *t = (gl_texture *)base;
	const gl_texture *src = (const gl_texture *)from;
	GLint was_fbo = 0;
	GLboolean was_scissor;

	if (gl.gl_major < 3 || t->texID == 0 || src->texID == 0 || src->texID == t->texID || columns == 0 || columns > base->w)
		return false;
	if (scale < 1)
		scale = 1;
	tex_cancel_decode(t);
	base->tex_type = TEX_TYPE_8888;
	if (!tex_take_pooled(t, base->w, base->h, false))
	{
		tex_release_pooled(t);
		glc_bind_texture(GL_TEXTURE_2D, t->texID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, base->w, base->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	was_scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (rtt_read_fbo == 0)
		glGenFramebuffers(1, &rtt_read_fbo);
	if (rtt_copy_fbo == 0)
		glGenFramebuffers(1, &rtt_copy_fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_read_fbo);
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src->texID, 0);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_copy_fbo);
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->texID, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, rtt_read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rtt_copy_fbo);
	if (was_scissor)
		glDisable(GL_SCISSOR_TEST);
	if (columns < base->w)
	{
		GLfloat was_clear[4];

		glGetFloatv(GL_COLOR_CLEAR_VALUE, was_clear);
		glClearColor(0.f, 0.f, 0.f, texcache_clear_opaque(base) ? 1.f : 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		glClearColor(was_clear[0], was_clear[1], was_clear[2], was_clear[3]);
	}
	glBlitFramebuffer(x * scale, y * scale, (x + columns) * scale, (y + base->h) * scale, 0, 0, columns, base->h, GL_COLOR_BUFFER_BIT,
			scale > 1 ? GL_LINEAR : GL_NEAREST);
	if (was_scissor)
		glEnable(GL_SCISSOR_TEST);
	/* (a texture attached to a framebuffer is not deleted when it is) */
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, rtt_copy_fbo);
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	rtt_gl_detach();
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
	return true;
#else
	(void)base; (void)from; (void)x; (void)y; (void)scale; (void)columns;
	return false;
#endif
}

static const texcache_ops gl_texture_ops =
{
	tex_get_id, tex_upload_to_gpu, tex_upload_memory, tex_gpu_decode, tex_part_memory, tex_upload_part,
	tex_gpu_decode_part, tex_copy_render, tex_force_32bit, tex_delete
};

/* rend/rtt_watch.h asks for a picture it left on the graphics card: w by h
 * pixels, whatever the card has it at. Called wherever the game first
 * touched the memory, the middle of a frame's set-up included, so what it
 * changes it puts back. (OpenGL ES 2: no buffer to read into; the pixels
 * into @rgba, and waited for.) */
static void rtt_gl_read(const RttWatch *watch, u8 *rgba)
{
	GLint was_fbo = 0, was_pack = 4;
	const u32 w = watch->w, h = watch->h, scale = watch->scale;
	u32 x0, y0;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &was_pack);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	if (rtt_gl_source(watch, &x0, &y0))
		glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	else
	{
		u32 y;
		/* no way to make it smaller on the card: the middle pixel of each
		 * (the renderer's thread's) */
		static u8 line[1024 * 8 * 4];

		for (y = 0; y < h; y++)
		{
			u32 x;
			glReadPixels(x0, y0 + y * scale + scale / 2, w * scale, 1, GL_RGBA, GL_UNSIGNED_BYTE, line);
			for (x = 0; x < w; x++)
				memcpy(rgba + (y * w + x) * 4, line + (x * scale + scale / 2) * 4, 4);
		}
	}
	rtt_gl_detach();
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, was_pack);
}

#ifndef HAVE_OPENGLES2
/* OpenGL 3 and OpenGL ES 3: copies of pictures off the card into one pixel
 * buffer, a ring - each copy its room in it, and a fence - begun ahead of
 * the read where rtt_watch says which (rtt_gl_prefetch()), or by the read,
 * and handed over from the buffer mapped, with no copy of this side's. The
 * ring is made with the renderer, four of a screen's render
 * (rtt_gl_init()); larger when a larger render is read, which is said. */
#define RTT_COPIES 18
#define RTT_RING_RENDERS 4

typedef struct GlRttCopy
{
	u32 serial;         /* the render's (RttWatch::serial), 0 if none */
	uintptr_t tex;
	u32 w, h;
	u64 stamp;          /* when it was last begun, for the oldest */
	size_t at, len;     /* its room in the ring; none if len is 0 */
	GLsync fence;
} GlRttCopy;

static GlRttCopy rtt_copies[RTT_COPIES];
static u64 rtt_stamp;
static GLuint rtt_ring;
static size_t rtt_ring_size, rtt_ring_head;

static bool rtt_gl_pbo(void)
{
	return gl.gl_major >= 3;
}

static GlRttCopy *rtt_gl_copy_of(const RttWatch *watch)
{
	int i;
	for (i = 0; i < RTT_COPIES; i++)
		if (rtt_copies[i].serial == watch->serial && rtt_copies[i].serial != 0 && rtt_copies[i].tex == watch->tex)
			return &rtt_copies[i];
	return NULL;
}

/* The ring, at least @bytes: made larger if it is not, every copy gone */
static void rtt_gl_ring(size_t bytes)
{
	int i;
	if (bytes <= rtt_ring_size)
		return;
	if (rtt_ring_size != 0 && !rtt_said_larger)
	{
		rtt_said_larger = true;
		WARN_LOG(RENDERER, "Render to a texture read back: its room made larger, %u KB\n", (u32)(bytes >> 10));
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		if (rtt_copies[i].fence != NULL)
			glDeleteSync(rtt_copies[i].fence);
		rtt_copies[i].fence = NULL;
		rtt_copies[i].serial = 0;
		rtt_copies[i].len = 0;
	}
	if (rtt_ring == 0)
		glGenBuffers(1, &rtt_ring);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, rtt_ring);
	glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
	rtt_ring_size = bytes;
	rtt_ring_head = 0;
}

/* One to begin a copy of @len bytes in, with its room in the ring: the
 * ring's next, from its start where the end is too near; what was there
 * before goes, and the copy that held none or was begun longest ago - not
 * after @since (in this batch) - gives its place. Null if what is there
 * was begun in this batch. */
static GlRttCopy *rtt_gl_copy_free(u64 since, size_t len)
{
	GlRttCopy *place = NULL;
	size_t at = rtt_ring_head;
	int i;

	if (len > rtt_ring_size)
		return NULL;
	if (at + len > rtt_ring_size)
		at = 0;
	for (i = 0; i < RTT_COPIES; i++)
	{
		const GlRttCopy *c = &rtt_copies[i];
		if (c->len != 0 && c->at < at + len && at < c->at + c->len && c->serial != 0 && c->stamp > since)
			return NULL;
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		GlRttCopy *c = &rtt_copies[i];
		if (c->len != 0 && c->at < at + len && at < c->at + c->len)
		{
			c->serial = 0;
			c->len = 0;
		}
		if (c->serial != 0 && c->stamp > since)
			continue;
		if (place == NULL || (place->len != 0 && (c->len == 0 || c->stamp < place->stamp)))
			place = c;
	}
	if (place == NULL)
		return NULL;
	place->at = at;
	place->len = len;
	rtt_ring_head = at + len;
	return place;
}

/* The copy of the picture into c's room in the ring, begun; false if it
 * cannot be (made smaller on the card) */
static bool rtt_gl_begin(const RttWatch *watch, GlRttCopy *c)
{
	u32 x0, y0;

	if (!rtt_gl_source(watch, &x0, &y0))
	{
		c->len = 0;
		return false;
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, rtt_ring);
	glReadPixels(x0, y0, watch->w, watch->h, GL_RGBA, GL_UNSIGNED_BYTE, (void *)(uintptr_t)c->at);
	if (c->fence != NULL)
		glDeleteSync(c->fence);
	c->fence = (GLsync)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	c->serial = watch->serial;
	c->tex = watch->tex;
	c->w = watch->w;
	c->h = watch->h;
	c->stamp = ++rtt_stamp;
	return true;
}

/* Copies of @watches begun, those that have one already left alone; what
 * a read changes put back */
static void rtt_gl_copies(const RttWatch *const *watches, unsigned count)
{
	unsigned i;
	GLint was_fbo = 0, was_pack = 4, was_pbo = 0;
	const u64 since = rtt_stamp;
	GLsync last = NULL;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &was_pack);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &was_pbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	for (i = 0; i < count; i++)
	{
		GlRttCopy *c;
		if (rtt_gl_copy_of(watches[i]) != NULL)
			continue;
		c = rtt_gl_copy_free(since, (size_t)watches[i]->w * watches[i]->h * 4);
		if (c == NULL)
			break;
		if (rtt_gl_begin(watches[i], c))
			last = c->fence;
	}
	if (last != NULL)
	{
		rtt_gl_detach();
		/* (on their way to the card now, not with whatever comes next: a
		 * wait of no time that flushes, which glsm has where it has no
		 * glFlush) */
		glClientWaitSync(last, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, was_pbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, was_pack);
}

static void rtt_gl_prefetch(const RttWatch *const *watches, unsigned count)
{
	if (!rtt_gl_pbo())
		return;
	/* (the room for the largest, made here where it is, not in a read: a
	 * render larger than a screen) */
	{
		unsigned i;
		size_t most = 0;
		u32 w = 0, h = 0;
		bool finer = false;
		GLint was_pbo = 0, was_fbo = 0;
		for (i = 0; i < count; i++)
		{
			const size_t len = (size_t)watches[i]->w * watches[i]->h * 4;
			if (len > most)
				most = len;
			if (watches[i]->w > w)
				w = watches[i]->w;
			if (watches[i]->h > h)
				h = watches[i]->h;
			finer = finer || watches[i]->scale > 1;
		}
		if (most * RTT_RING_RENDERS > rtt_ring_size)
		{
			glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &was_pbo);
			rtt_gl_ring(most * RTT_RING_RENDERS);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, was_pbo);
		}
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(GL_RGBA8)
		if (finer && (w > rtt_small_w || h > rtt_small_h))
		{
			glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
			rtt_gl_small(w, h);
			glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
		}
#endif
		(void)finer;
		(void)was_fbo;
	}
	rtt_gl_copies(watches, count);
}

/* The picture, from its copy's buffer mapped: begun ahead as often as not,
 * and done by now; or begun here and waited for */
static bool rtt_gl_take_copy(const RttWatch *watch, RttWatchTake take)
{
	GlRttCopy *c = rtt_gl_copy_of(watch);
	GLint was_pbo = 0;
	const u8 *p;

	if (c == NULL)
	{
		rtt_gl_copies(&watch, 1);
		c = rtt_gl_copy_of(watch);
		if (c == NULL)
			return false;
	}
	glClientWaitSync(c->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0xFFFFFFFFFFFFFFFFull);
	glDeleteSync(c->fence);
	c->fence = NULL;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &was_pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, rtt_ring);
	p = (const u8 *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, (GLintptr)c->at, (GLsizeiptr)c->w * c->h * 4,
			GL_MAP_READ_BIT);
	if (p != NULL)
	{
		take(watch, p, (size_t)c->w * 4);
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, was_pbo);
	c->serial = 0;
	c->len = 0;
	return p != NULL;
}
#endif

/* the context is going: what the reads kept */
static void rtt_gl_term(void)
{
#ifndef HAVE_OPENGLES2
	int i;
	for (i = 0; i < RTT_COPIES; i++)
	{
		if (rtt_copies[i].fence != NULL)
			glDeleteSync(rtt_copies[i].fence);
		memset(&rtt_copies[i], 0, sizeof(rtt_copies[i]));
	}
	if (rtt_ring != 0)
		glDeleteBuffers(1, &rtt_ring);
	rtt_ring = 0;
	rtt_ring_size = rtt_ring_head = 0;
#endif
	if (rtt_read_fbo != 0)
		glDeleteFramebuffers(1, &rtt_read_fbo);
	if (rtt_copy_fbo != 0)
		glDeleteFramebuffers(1, &rtt_copy_fbo);
	if (rtt_small_fbo != 0)
		glDeleteFramebuffers(1, &rtt_small_fbo);
	if (rtt_small_rb != 0)
		glDeleteRenderbuffers(1, &rtt_small_rb);
	rtt_read_fbo = rtt_copy_fbo = rtt_small_fbo = rtt_small_rb = 0;
	rtt_small_w = rtt_small_h = 0;
	rtt_said_larger = false;
}

/* What a read back takes, made with the renderer for renders of a screen's
 * size: the framebuffer it reads through, the ring (OpenGL 3 and OpenGL ES
 * 3), the picture a finer render is brought down to (with Render To Texture
 * Upscaling, OpenGL 3) */
static void rtt_gl_init(void)
{
	GLint was_fbo = 0;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	if (rtt_read_fbo == 0)
		glGenFramebuffers(1, &rtt_read_fbo);
	if (rtt_copy_fbo == 0 && gl.gl_major >= 3)
		glGenFramebuffers(1, &rtt_copy_fbo);
#ifndef HAVE_OPENGLES2
	if (rtt_gl_pbo())
	{
		GLint was_pbo = 0;
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &was_pbo);
		rtt_gl_ring((size_t)640 * 480 * 4 * RTT_RING_RENDERS);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, was_pbo);
	}
#endif
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(GL_RGBA8)
	if (settings.rend.RenderToTextureUpscale > 1 && gl.gl_major >= 3)
		rtt_gl_small(640, 480);
#endif
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
}

static void rtt_gl_release(uintptr_t tex)
{
	GLuint id = (GLuint)tex;
	/* (the pool's goes back to it) */
	GLPooledTexture *pooled = pool_find(id);
	if (pooled != NULL)
		gl_tex_pool_give(pooled);
	else
		glc_delete_textures(1, &id);
}

/* (from a pixel buffer where there are any; or read back to memory of this
 * side's, and handed over from there) */
static void rtt_gl_take(const RttWatch *watch, u8 *scratch, RttWatchTake take)
{
#ifndef HAVE_OPENGLES2
	if (rtt_gl_pbo() && rtt_gl_take_copy(watch, take))
		return;
#endif
	rtt_gl_read(watch, scratch);
	take(watch, scratch, (size_t)watch->w * 4);
}

#ifndef HAVE_OPENGLES2
static const RttWatchBackend rtt_gl_backend = { rtt_gl_take, rtt_gl_release, rtt_gl_prefetch };
#else
static const RttWatchBackend rtt_gl_backend = { rtt_gl_take, rtt_gl_release };
#endif

/* rend/last_picture.h: the context is going, and the screen's last picture
 * is in the framebuffer the frontend gave to draw in. */
void gl_keep_picture(void)
{
	GLint was_fbo = 0, was_pack = 4;
	u8 *pixels;
	u8 *line;
	const int w = screen_width, h = screen_height;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	/* (what cannot be put back is not read: see gl_restore_picture()) */
	if (gl.gl_major < 3)
		return;
#else
	return;
#endif
	pixels = last_picture_keep(w, h);

	if (pixels == NULL)
		return;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &was_pack);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, was_pack);

	/* read bottom line first; kept top line first */
	line = (u8 *)malloc((size_t)w * 4);
	if (line != NULL)
	{
		int y;
		for (y = 0; y < h / 2; y++)
		{
			u8 *a = pixels + (size_t)y * w * 4, *b = pixels + (size_t)(h - 1 - y) * w * 4;
			memcpy(line, a, (size_t)w * 4);
			memcpy(a, b, (size_t)w * 4);
			memcpy(b, line, (size_t)w * 4);
		}
		free(line);
	}
}

/* The first render to the screen in a new context starts from the picture
 * kept from the old one: it goes into the framebuffer that is bound, the
 * one the render is about to draw in, at that one's size. */
void gl_restore_picture(void)
{
	GLboolean was_scissor;
	GLuint fbo;
	GLint target;
	GLuint tex;
	int w, h;
	const u8 *pixels = last_picture(&w, &h);

	if (pixels == NULL)
		return;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	if (gl.gl_major < 3)
		return;

	target = 0;
	fbo = 0;
	was_scissor = glIsEnabled(GL_SCISSOR_TEST);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
	tex = glc_gen_texture();
	glc_bind_texture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	if (was_scissor)
		glDisable(GL_SCISSOR_TEST);
	/* the texture's first line is the picture's top one: upside down on the way */
	glBlitFramebuffer(0, 0, w, h, 0, screen_height, screen_width, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	if (was_scissor)
		glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, target);
	glDeleteFramebuffers(1, &fbo);
	glc_delete_textures(1, &tex);
#endif
}

/* The size rendered at changes and the context stays (Renderer::Resize()).
 * The picture is in the frontend's buffer, in the corner of it that was the
 * old size; a game that draws part of the screen needs it in the corner
 * that is the new size. It is scaled from the one to the other on the
 * graphics card - by way of a texture, the two overlap - and never leaves
 * it. (Not with OpenGL ES 2, which cannot do it or put a kept picture
 * back either.) */
void gl_carry_picture(int from_w, int from_h, int to_w, int to_h)
{
	if (from_w <= 0 || from_h <= 0 || (from_w == to_w && from_h == to_h))
		return;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	if (gl.gl_major >= 3)
	{
		GLuint tex;
		GLint was_fbo = 0;
		GLuint fbo = 0;
		const GLuint target = hw_render.get_current_framebuffer();
		const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
		tex = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, to_w, to_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &fbo);
		if (was_scissor)
			glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
		glBlitFramebuffer(0, 0, from_w, from_h, 0, 0, to_w, to_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
		glBlitFramebuffer(0, 0, to_w, to_h, 0, 0, to_w, to_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if (was_scissor)
			glEnable(GL_SCISSOR_TEST);
		glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
		glDeleteFramebuffers(1, &fbo);
		glc_delete_textures(1, &tex);
	}
#endif
}

/* OpenGL ES 2 promises a render to a 5-6-5 texture and to nothing finer:
 * set once a driver has refused one of eight bits a channel. */
static bool rtt_rgb_bytes_refused;

void BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt)
{
   u32 fbh2;
   u32 fbw2;
	GLuint uStatus;
	if (rtt_rgb_bytes_refused && channels == GL_RGB)
		fmt = GL_UNSIGNED_SHORT_5_6_5;

	/* (with the pool, the framebuffer and the depth buffer are kept, and
	 * the texture is the pool's) */
	if (gl.rtt.fbo && !gl.rtt.kept)
      glDeleteFramebuffers(1,&gl.rtt.fbo);
	if (gl.rtt.pooled != NULL)
		gl_tex_pool_give(gl.rtt.pooled);
	else if (gl.rtt.tex)
      glc_delete_textures(1,&gl.rtt.tex);
	gl.rtt.pooled = NULL;
	gl.rtt.tex = 0;
	if (gl.rtt.depthb && !gl.rtt.kept)
      glDeleteRenderbuffers(1,&gl.rtt.depthb);

	gl.rtt.TexAddr=addy>>3;

	/* Find the smallest power of two texture that fits the viewport */
   fbh2 = 2;
   while (fbh2 < fbh)
      fbh2 *= 2;
   fbw2 = 2;
   while (fbw2 < fbw)
      fbw2 *= 2;

   if (settings.rend.RenderToTextureUpscale > 1)
	{
		fbw *= settings.rend.RenderToTextureUpscale;
		fbh *= settings.rend.RenderToTextureUpscale;
		fbw2 *= settings.rend.RenderToTextureUpscale;
		fbh2 *= settings.rend.RenderToTextureUpscale;
	}

	/* Get the currently bound frame buffer object. On most platforms this just gives 0. */

	if (gl.rtt.kept && (fbw2 > gl.rtt.depthWidth || fbh2 > gl.rtt.depthHeight))
	{
		/* (a render larger than the kept depth buffer: one of that size,
		 * and kept from then on) */
		WARN_LOG(RENDERER, "Render to a texture of %u x %u: its depth buffer made larger\n", fbw2, fbh2);
		glDeleteRenderbuffers(1, &gl.rtt.depthb);
		gl.rtt.depthb = 0;
	}
	if (!gl.rtt.kept || gl.rtt.depthb == 0)
	{
		/* Generate and bind a render buffer which will become a depth buffer shared between our two FBOs */
		glGenRenderbuffers(1, &gl.rtt.depthb);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, gl.rtt.depthb);

		/*
			Currently it is unknown to GL that we want our new render buffer to be a depth buffer.
			glRenderbufferStorage will fix this and in this case will allocate a depth buffer
			m_i32TexSize by m_i32TexSize.
		*/

		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, gl.depth_format, fbw2, fbh2);
		gl.rtt.depthWidth = fbw2;
		gl.rtt.depthHeight = fbh2;
	}

	/* A texture for rendering to: the pool's, made at the start, if it has one of that shape */
	if (fmt == GL_UNSIGNED_BYTE)
		gl.rtt.pooled = gl_tex_pool_take_target(channels == GL_RGB, fbw2, fbh2);
	if (gl.rtt.pooled != NULL)
		gl.rtt.tex = gl.rtt.pooled->name;
	else
	{
		gl.rtt.tex = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, gl.rtt.tex);

		glTexImage2D(GL_TEXTURE_2D, 0, channels, fbw2, fbh2, 0, channels, fmt, 0);
	}

	/* Create the object that will allow us to render to the aforementioned texture */
	if (!gl.rtt.kept)
		glGenFramebuffers(1, &gl.rtt.fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, gl.rtt.fbo);

	/* Attach the texture to the FBO */
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.rtt.tex, 0);

	/* Attach the depth buffer we created earlier to our FBO. */
#if defined(HAVE_OPENGLES2) || defined(HAVE_OPENGLES1) || defined(OSX_PPC)
	glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_DEPTH_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
   glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_STENCIL_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
#else
   glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
#endif

	/* Check that our FBO creation was successful */
	uStatus = glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER);

	if (uStatus != RARCH_GL_FRAMEBUFFER_COMPLETE && channels == GL_RGB && fmt == GL_UNSIGNED_BYTE)
	{
		rtt_rgb_bytes_refused = true;
		if (gl.rtt.pooled != NULL)
		{
			/* (the pool's goes back: one of the cache's own, made as below) */
			gl_tex_pool_give(gl.rtt.pooled);
			gl.rtt.pooled = NULL;
			gl.rtt.tex = glc_gen_texture();
			glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.rtt.tex, 0);
		}
		/* (the texture is still the one bound, and stays attached) */
		glc_bind_texture(GL_TEXTURE_2D, gl.rtt.tex);
		glTexImage2D(GL_TEXTURE_2D, 0, channels, fbw2, fbh2, 0, channels, GL_UNSIGNED_SHORT_5_6_5, 0);
		uStatus = glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER);
	}

	verify(uStatus == RARCH_GL_FRAMEBUFFER_COMPLETE);

   glViewport(0, 0, fbw, fbh);		/* TODO CLIP_X/Y min? */
}

void ReadRTTBuffer(void)
{
	u8 fb_packmode;
	RttWatch watch;
	/* The texture is the frame from its first pixel (see TransformMatrix);
	 * what goes to video memory is the pixel clip's part of it, at its
	 * place in the frame. */
	const u32 x0 = pvrrc.fb_X_CLIP.min;
	const u32 y0 = pvrrc.fb_Y_CLIP.min;
	const u32 tex_w = pvrrc.fb_X_CLIP.max + 1;
	const u32 tex_h = pvrrc.fb_Y_CLIP.max + 1;
	u32 w = tex_w - x0;
	u32 h = tex_h - y0;

	u32 stride = FB_W_LINESTRIDE.stride * 8;
	if (stride == 0)
		stride = tex_w * 2;
	else if (tex_w * 2 > stride)
		/* (Virtua Tennis) */
		w = x0 * 2 >= stride ? 0 : stride / 2 - x0;

	fb_packmode = FB_W_CTRL.fb_packmode;

	/* The picture stays on the graphics card, and video memory gets it when
	 * something first touches it there: see rend/rtt_watch.h. */
	memset(&watch, 0, sizeof(watch));
	watch.addr = (gl.rtt.TexAddr << 3) + y0 * stride + x0 * 2;
	watch.w = w;
	watch.h = h;
	watch.x0 = x0;
	watch.y0 = y0;
	watch.stride = stride;
	watch.bytes = h == 0 || w == 0 ? 0 : stride * (h - 1) + w * 2;
	watch.packmode = fb_packmode;
	watch.kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
	watch.alpha_threshold = FB_W_CTRL.fb_alpha_threshold;
	watch.dither = FB_W_CTRL.fb_dither;
	watch.tex = gl.rtt.tex;
	watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (watch.bytes != 0)
		rtt_watch_supersede(watch.addr, watch.bytes);

	if (tex_w > 1024 || tex_h > 1024)
	{
		/* too large to be drawn with from here: kept for video memory alone */
		watch.owned = true;
		rtt_watch_add(&watch, &rtt_gl_backend);
	}
	else
	{
		/* TexAddr, ScanOrder 1 */
		u32 tcw = TAW_SET(TAW_SET(0, TCW_TexAddr, gl.rtt.TexAddr), TCW_ScanOrder, 1);
		u32 tsp = 0, u, v;
		gl_texture *texture_data;
		switch (fb_packmode)
		{
		case 0:
		case 3:
			tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_1555);
			break;
		case 1:
			tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_565);
			break;
		case 2:
			tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_4444);
			break;
		}
		for (u = 0; u <= 7 && (8u << u) < tex_w; u++)
			;
		for (v = 0; v <= 7 && (8u << v) < tex_h; v++)
			;
		tsp = TAW_SET(TAW_SET(tsp, TSP_TexU, u), TSP_TexV, v);

		texture_data = gl_texture_of(tsp, tcw, 0);
		if (texture_data->texID == 0)
			texcache_create(&texture_data->base);
		tex_set_rendered(texture_data, gl.rtt.tex, gl.rtt.pooled);
		texture_data->base.dirty = 0;
		texture_data->base.stride_read = 0;	/* (rendered, not read) */
		texture_data->base.made_type = 0;
		libCore_vramlock_Lock(texture_data->base.sa_tex, texture_data->base.sa + texture_data->base.size - 1, &texture_data->base);
		watch.target = &texture_data->base;
	  rtt_watch_add(&watch, &rtt_gl_backend);
	}
	gl.rtt.tex = 0;
	gl.rtt.pooled = NULL;

	if (gl.rtt.fbo && !gl.rtt.kept) { glDeleteFramebuffers(1,&gl.rtt.fbo); gl.rtt.fbo = 0; }
	if (gl.rtt.depthb && !gl.rtt.kept) { glDeleteRenderbuffers(1,&gl.rtt.depthb); gl.rtt.depthb = 0; }

	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
}

static int TexCacheLookups;
static int TexCacheHits;
static float LastTexCacheStats;


u64 gl_get_texture(u32 tsp, u32 tcw, int area)
{
	gl_texture *tf = gl_texture_of(tsp, tcw, area);

	TexCacheLookups++;
	if (tf->texID == 0)
	{
		texcache_create(&tf->base);
		tf->texID = glc_gen_texture();
	}

	/* update if needed */
	if (texcache_needs_update(&tf->base))
		texcache_update(&tf->base);
	else
	{
		if (texcache_custom_available(&tf->base))
		{
			tex_renew(tf);
			texcache_check_custom(&tf->base);
		}
		TexCacheHits++;
	}

	return tf->texID;
}

GLuint fbTextureId;
/* the picture's size the texture was made for; the buffer it is converted
 * into, mapped (OpenGL 3, OpenGL ES 3), and how large; or, without one,
 * memory of this side's, kept */
static int fbTextureW, fbTextureH;
static GLuint fbUnpack;
static size_t fbUnpackSize;
static grow_buf_t fbScratch;

/* The framebuffer the game wrote, converted straight into the memory the
 * texture is made from: a buffer of the card's, mapped, where there is one.
 * The texture is kept from one frame to the next and made again only for
 * another size. */
void RenderFramebuffer(void)
{
	FramebufferRead fb;
	size_t bytes;
	int width, height;
	u8 *scratch;
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return;

	ReadFramebufferGeometry(&fb);
	width = fb.width;
	height = fb.height;
	bytes = (size_t)width * height * 4;

	if (fbTextureId == 0)
	{
		fbTextureId = glc_gen_texture();
		fbTextureW = fbTextureH = 0;
	}
	glc_bind_texture(GL_TEXTURE_2D, fbTextureId);
	if (fbTextureW != width || fbTextureH != height)
	{
		/* set texture repeat mode */
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		fbTextureW = width;
		fbTextureH = height;
	}
#ifndef HAVE_OPENGLES2
	if (gl.gl_major >= 3)
	{
		u8 *p;
		if (fbUnpack == 0)
			glGenBuffers(1, &fbUnpack);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, fbUnpack);
		if (fbUnpackSize < bytes)
		{
			glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, NULL, GL_STREAM_DRAW);
			fbUnpackSize = bytes;
		}
		/* (whatever the card still reads of the last one is its own: the
		 * buffer's memory is new) */
		p = (u8 *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)bytes,
				GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
		if (p != NULL)
		{
			ReadFramebufferTo(&fb, p, (size_t)width * 4);
			glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			return;
		}
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
#endif
	scratch = (u8 *)grow_buf_reserve(&fbScratch, bytes);
	if (scratch == NULL)
		return;
	ReadFramebufferTo(&fb, scratch, (size_t)width * 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
}

/* the context is going: what RenderFramebuffer() kept (the texture goes
 * with the renderer's) */
static void fb_texture_term(void)
{
	if (fbUnpack != 0)
		glDeleteBuffers(1, &fbUnpack);
	fbUnpack = 0;
	fbUnpackSize = 0;
	fbTextureW = fbTextureH = 0;
	grow_buf_free(&fbScratch);
}

#if 0
/* currently not needed, but perhaps for soft rendering */
void render_vmu_screen(u8* screen_data, u32 width, u32 height, u8 vmu_screen_to_display)
{
	u8 *dst = screen_data;
	u8 *src = NULL ;
	u32 line_size = width*4 ;
	u32 start_offset ;
	u32 x,y ;

	src = vmu_screen_params[vmu_screen_to_display].vmu_lcd_screen ;

	if ( src == NULL )
		return ;

	switch ( vmu_screen_params[vmu_screen_to_display].vmu_screen_position )
	{
		case UPPER_LEFT :
		{
			start_offset = 0 ;
			break ;
		}
		case UPPER_RIGHT :
		{
			start_offset = line_size - (VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_to_display].vmu_screen_size_mult*4) ;
			break ;
		}
		case LOWER_LEFT :
		{
			start_offset = line_size*(height - VMU_SCREEN_HEIGHT) ;
			break ;
		}
		case LOWER_RIGHT :
		{
			start_offset = line_size*(height - VMU_SCREEN_HEIGHT) + (line_size - (VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_to_display].vmu_screen_size_mult*4));
			break ;
		}
	}


	for ( y = 0 ; y < VMU_SCREEN_HEIGHT ; y++)
	{
		dst = screen_data + start_offset + (y*line_size);
		for ( x = 0 ; x < VMU_SCREEN_WIDTH ; x++)
		{
			if ( *src++ > 0 )
			{
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_on_R ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_on_G ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_on_B ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_screen_opacity ;
			}
			else
			{
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_off_R ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_off_G ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_off_B ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_screen_opacity ;
			}
		}
	}
}
#endif
