/* A libretro frontend that draws with a real OpenGL and no screen: an EGL
 * context on Mesa's surfaceless platform (llvmpipe will do), a framebuffer
 * of its own for the core to draw in, and the last frame read back and
 * written out as vk_hdr.c writes its pictures - a 16-byte header ("VKHR",
 * the Vulkan format 37 for R8G8B8A8, the width, the height) and the rows,
 * top first - so that the OpenGL renderers' pictures can be judged by the
 * same scripts as the Vulkan ones' (tools/gl/gl_dma.sh).
 *
 *   gl_frames CORE CONTENT OUT.raw FRAMES [key=value ...]
 *
 * key=value pairs are core options. GLF_DIR is the system/save directory
 * (/tmp by default); GLF_GLES=1 offers an OpenGL ES 3 context instead of
 * an OpenGL core one; GLF_EVERY=1 prints a checksum of every frame the
 * core hands over, "frame N SIZE SUM", for runs to be compared frame for
 * frame (the frames a game writes into its framebuffer itself among them,
 * which a run's last frame seldom is). GLF_AHEAD=n runs the frames from
 * the n-th on as a frontend's run-ahead of one frame does: the frame, a
 * state saved, the frame after it (the one shown) and the state loaded
 * back. The picture is then the one a plain run of one frame more shows,
 * for whatever a state carries (tools/gl/gl_ahead.sh). GLF_READS=1 counts
 * what the core reads back from the card - a buffer mapped to be read, a
 * glReadPixels - and prints "reads N" at the end (tools/gl/gl_dma.sh); a
 * buffer mapped once for good (persistently) is not a read of a frame.
 * GLF_NO_STORAGE=1: no GL_ARB_buffer_storage, as an OpenGL older than 4.4
 * has it: the frame's lists are copied to the card, not parsed where it
 * reads them (tools/gl/gl_parse_arena.sh). GLF_VERSION=gles2 or gl2: the
 * context said to be OpenGL ES 2.0 (with GLF_GLES) or OpenGL 2.1, so the
 * core takes its paths for those (their shaders still compile on the newer
 * context). GLF_NO_PARALLEL=1: no KHR_ or ARB_parallel_shader_compile.
 * GLF_SLOW_COMPILE=1: a driver that compiles in the background, as the core
 * sees it - a program's first GL_COMPLETION_STATUS_KHR is "not done".
 * GLF_COUNT_PROGRAMS=1 prints "programs: C compiled, L linked, B loaded in
 * retro_run" - glCompileShader, glLinkProgram and glProgramBinary called
 * inside it (tools/gl/gl_draw_path.sh).
 * GLF_ALLOCS=N, built with -DALLOC_COUNT -rdynamic and
 * tools/rend/alloc_count.c: the core's allocations during play, from frame
 * N on (AGENTS.md, "No allocations during play"), printed after the
 * frames - its heap's, and its OpenGL objects': names made (glGen*),
 * storage given (glTexImage*, glTexStorage*, glRenderbufferStorage*,
 * glBufferStorage, a glBufferData larger than the buffer ever was), programs
 * and shaders made; with GLF_ALLOC_SITES, where in the core each was
 * (tools/gl/gl_allocs.sh).
 *
 * Built with: cc -rdynamic -o gl_frames gl_frames.c -lEGL -lGL -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../../core/libretro-common/include/libretro.h"

static struct retro_hw_render_callback hw;
static int have_hw, gles;
static char **options;
static int option_count;
static unsigned last_w, last_h;

static GLuint fbo, color, depth;
static unsigned fb_w, fb_h;

static PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
static PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
static PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;

static void log_line(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("GLF_VERBOSE"))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static uintptr_t gl_framebuffer(void) { return fbo; }
static long reads;
static void *(*real_map_range)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*real_read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void *count_map_range(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
   if ((access & GL_MAP_READ_BIT) && !(access & GL_MAP_PERSISTENT_BIT))
      reads++;
   return real_map_range(target, offset, length, access);
}
static void count_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels)
{
   reads++;
   real_read_pixels(x, y, w, h, format, type, pixels);
}

static int hidden(const char *e)
{
   if (getenv("GLF_NO_STORAGE") && !strcmp(e, "GL_ARB_buffer_storage"))
      return 1;
   return getenv("GLF_NO_PARALLEL") && (!strcmp(e, "GL_KHR_parallel_shader_compile")
         || !strcmp(e, "GL_ARB_parallel_shader_compile"));
}

static const GLubyte *(*real_get_stringi)(GLenum, GLuint);
static const GLubyte *hide_get_stringi(GLenum name, GLuint i)
{
   const GLubyte *e = real_get_stringi(name, i);
   if (name == GL_EXTENSIONS && e != NULL && hidden((const char *)e))
      return (const GLubyte *)"GL_hidden_extension";
   return e;
}

/* GLF_VERSION and GLF_NO_PARALLEL: what the context says it is. The core
 * calls these two of OpenGL 1 straight, not through get_proc_address:
 * they are defined here, and bound to the core's calls when this is built
 * with -rdynamic (the core is loaded after it). */
static const GLubyte *(*real_get_string)(GLenum);
static char extensions[65536];
static const GLubyte *version_get_string(GLenum name)
{
   const char *v = getenv("GLF_VERSION");
   if (!real_get_string)
      *(void **)&real_get_string = dlsym(RTLD_NEXT, "glGetString");
   if (name == GL_VERSION && v)
      return (const GLubyte *)(!strcmp(v, "gles2") ? "OpenGL ES 2.0 (gl_frames)" : "2.1 (gl_frames)");
   if (name == GL_EXTENSIONS && getenv("GLF_NO_PARALLEL"))
   {
      /* the list without the hidden ones */
      const char *all = (const char *)real_get_string(name);
      char word[256];
      size_t n = 0, at = 0;
      extensions[0] = '\0';
      while (all != NULL && all[at] != '\0')
      {
         size_t len = strcspn(all + at, " ");
         if (len > 0 && len < sizeof(word))
         {
            memcpy(word, all + at, len);
            word[len] = '\0';
            if (!hidden(word) && n + len + 2 < sizeof(extensions))
            {
               memcpy(extensions + n, word, len);
               n += len;
               extensions[n++] = ' ';
               extensions[n] = '\0';
            }
         }
         at += len;
         while (all[at] == ' ')
            at++;
      }
      return (const GLubyte *)extensions;
   }
   return real_get_string(name);
}
static void (*real_get_integerv)(GLenum, GLint *);
static void version_get_integerv(GLenum name, GLint *v)
{
   const char *ver = getenv("GLF_VERSION");
   if (!real_get_integerv)
      *(void **)&real_get_integerv = dlsym(RTLD_NEXT, "glGetIntegerv");
   if (ver && name == GL_MAJOR_VERSION)
      *v = 2;
   else if (ver && name == GL_MINOR_VERSION)
      *v = !strcmp(ver, "gles2") ? 0 : 1;
   else
      real_get_integerv(name, v);
}

const GLubyte *GLAPIENTRY glGetString(GLenum name)
{
   return version_get_string(name);
}
void GLAPIENTRY glGetIntegerv(GLenum name, GLint *v)
{
   version_get_integerv(name, v);
}

/* GLF_SLOW_COMPILE */
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif
static unsigned char asked_done[65536];
static void (*real_get_programiv)(GLuint, GLenum, GLint *);
static void slow_get_programiv(GLuint p, GLenum name, GLint *v)
{
   real_get_programiv(p, name, v);
   if (name == GL_COMPLETION_STATUS_KHR && p < sizeof(asked_done) && !asked_done[p])
   {
      asked_done[p] = 1;
      *v = 0;
   }
}

/* GLF_COUNT_PROGRAMS: what retro_run compiles, links and loads */
static int in_run;
static long compiled, linked, loaded;
static void (*real_compile)(GLuint);
static void count_compile(GLuint s) { if (in_run) compiled++; real_compile(s); }
static void (*real_link)(GLuint);
static void count_link(GLuint p) { if (in_run) linked++; real_link(p); }
static void (*real_binary)(GLuint, GLenum, const void *, GLsizei);
static void count_binary(GLuint p, GLenum f, const void *b, GLsizei n) { if (in_run) loaded++; real_binary(p, f, b, n); }

#ifdef ALLOC_COUNT
#include "../rend/alloc_count.h"
/* GLF_ALLOCS: the OpenGL objects the core makes and gives storage, from its
 * own code (alloc_count.c names where) */
enum { GLF_A_NAMES = ALLOC_HOST_KINDS, GLF_A_STORAGE, GLF_A_PROGRAMS, GLF_A_KINDS };
static const char *const glf_a_names[GLF_A_KINDS] = { "heap allocations", "heap frees", "objects named",
   "storage given", "programs and shaders made" };
static const char *const glf_a_site[GLF_A_KINDS] = { "heap", NULL, "gl", "gl", "gl" };
#define GLF_SEEN(kind) alloc_count_seen(kind, __builtin_return_address(0))

/* the most a buffer has had, by its name, from the binding of its target:
 * a glBufferData no larger than that - a frame's data streamed, the
 * buffer's storage given anew, which a driver keeps for it - asks for
 * none more */
#define GLF_BUFFERS 65536
static GLsizeiptr glf_buffer_size[GLF_BUFFERS];
static GLuint glf_bound[16];
static int glf_target(GLenum t)
{
   static const GLenum targets[] = { GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_UNIFORM_BUFFER,
      GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
      GL_SHADER_STORAGE_BUFFER, GL_TEXTURE_BUFFER, GL_DRAW_INDIRECT_BUFFER, GL_ATOMIC_COUNTER_BUFFER,
      GL_TRANSFORM_FEEDBACK_BUFFER, GL_DISPATCH_INDIRECT_BUFFER, GL_QUERY_BUFFER };
   int i;
   for (i = 0; i < (int)(sizeof(targets) / sizeof(targets[0])); i++)
      if (targets[i] == t)
         return i;
   return 15;
}

#define GLF_NAMER(fn) \
   static void (*real_##fn)(GLsizei, GLuint *); \
   static void count_##fn(GLsizei n, GLuint *names) { GLF_SEEN(GLF_A_NAMES); real_##fn(n, names); }
GLF_NAMER(glGenTextures)
GLF_NAMER(glGenBuffers)
GLF_NAMER(glGenFramebuffers)
GLF_NAMER(glGenRenderbuffers)
GLF_NAMER(glGenVertexArrays)
GLF_NAMER(glGenQueries)
GLF_NAMER(glGenSamplers)
static void (*real_glBindBuffer)(GLenum, GLuint);
static void count_glBindBuffer(GLenum t, GLuint b) { glf_bound[glf_target(t)] = b; real_glBindBuffer(t, b); }
static void (*real_glBindBufferBase)(GLenum, GLuint, GLuint);
static void count_glBindBufferBase(GLenum t, GLuint i, GLuint b) { glf_bound[glf_target(t)] = b; real_glBindBufferBase(t, i, b); }
static void (*real_glBindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static void count_glBindBufferRange(GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr n)
{
   glf_bound[glf_target(t)] = b;
   real_glBindBufferRange(t, i, b, o, n);
}
static void (*real_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void count_glBufferData(GLenum t, GLsizeiptr n, const void *d, GLenum u)
{
   const GLuint b = glf_bound[glf_target(t)];
   if (b < GLF_BUFFERS)
   {
      if (n > glf_buffer_size[b])
      {
         GLF_SEEN(GLF_A_STORAGE);
         glf_buffer_size[b] = n;
      }
   }
   real_glBufferData(t, n, d, u);
}
static void (*real_glBufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);
static void count_glBufferStorage(GLenum t, GLsizeiptr n, const void *d, GLbitfield f)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glBufferStorage(t, n, d, f);
}
static void (*real_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void count_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
      const void *d)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glTexImage2D(t, l, i, w, h, b, f, ty, d);
}
static void (*real_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void count_glTexImage3D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLsizei dp, GLint b, GLenum f,
      GLenum ty, const void *d)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glTexImage3D(t, l, i, w, h, dp, b, f, ty, d);
}
static void (*real_glTexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void count_glTexStorage2D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glTexStorage2D(t, l, f, w, h);
}
static void (*real_glTexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
static void count_glTexStorage3D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glTexStorage3D(t, l, f, w, h, d);
}
static void (*real_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void count_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glRenderbufferStorage(t, f, w, h);
}
static void (*real_glRenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void count_glRenderbufferStorageMultisample(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)
{
   GLF_SEEN(GLF_A_STORAGE);
   real_glRenderbufferStorageMultisample(t, s, f, w, h);
}
static GLuint (*real_glCreateProgram)(void);
static GLuint count_glCreateProgram(void) { GLF_SEEN(GLF_A_PROGRAMS); return real_glCreateProgram(); }
static GLuint (*real_glCreateShader)(GLenum);
static GLuint count_glCreateShader(GLenum t) { GLF_SEEN(GLF_A_PROGRAMS); return real_glCreateShader(t); }

static retro_proc_address_t glf_alloc_wrap(const char *sym, retro_proc_address_t f)
{
#define GLF_WRAP(fn) \
   if (!strcmp(sym, #fn)) { *(retro_proc_address_t *)&real_##fn = f; return (retro_proc_address_t)count_##fn; }
   GLF_WRAP(glGenTextures)
   GLF_WRAP(glGenBuffers)
   GLF_WRAP(glGenFramebuffers)
   GLF_WRAP(glGenRenderbuffers)
   GLF_WRAP(glGenVertexArrays)
   GLF_WRAP(glGenQueries)
   GLF_WRAP(glGenSamplers)
   GLF_WRAP(glBindBuffer)
   GLF_WRAP(glBindBufferBase)
   GLF_WRAP(glBindBufferRange)
   GLF_WRAP(glBufferData)
   GLF_WRAP(glBufferStorage)
   GLF_WRAP(glTexImage2D)
   GLF_WRAP(glTexImage3D)
   GLF_WRAP(glTexStorage2D)
   GLF_WRAP(glTexStorage3D)
   GLF_WRAP(glRenderbufferStorage)
   GLF_WRAP(glRenderbufferStorageMultisample)
   GLF_WRAP(glCreateProgram)
   GLF_WRAP(glCreateShader)
#undef GLF_WRAP
   return f;
}
#endif

static retro_proc_address_t gl_proc(const char *sym)
{
   retro_proc_address_t f = (retro_proc_address_t)eglGetProcAddress(sym);
#ifdef ALLOC_COUNT
   if (getenv("GLF_ALLOCS") && f != NULL)
   {
      retro_proc_address_t w = glf_alloc_wrap(sym, f);
      if (w != f)
         return w;
   }
#endif
   if ((getenv("GLF_NO_STORAGE") || getenv("GLF_NO_PARALLEL")) && f != NULL && !strcmp(sym, "glGetStringi"))
   {
      *(retro_proc_address_t *)&real_get_stringi = f;
      return (retro_proc_address_t)hide_get_stringi;
   }
   if (!strcmp(sym, "glGetString"))
      return (retro_proc_address_t)glGetString;
   if (!strcmp(sym, "glGetIntegerv"))
      return (retro_proc_address_t)glGetIntegerv;
   if (getenv("GLF_SLOW_COMPILE") && f != NULL && !strcmp(sym, "glGetProgramiv"))
   {
      *(retro_proc_address_t *)&real_get_programiv = f;
      return (retro_proc_address_t)slow_get_programiv;
   }
   if (getenv("GLF_COUNT_PROGRAMS") && f != NULL)
   {
      if (!strcmp(sym, "glCompileShader"))
      {
         *(retro_proc_address_t *)&real_compile = f;
         return (retro_proc_address_t)count_compile;
      }
      if (!strcmp(sym, "glLinkProgram"))
      {
         *(retro_proc_address_t *)&real_link = f;
         return (retro_proc_address_t)count_link;
      }
      if (!strcmp(sym, "glProgramBinary"))
      {
         *(retro_proc_address_t *)&real_binary = f;
         return (retro_proc_address_t)count_binary;
      }
   }
   if (!getenv("GLF_READS") || f == NULL)
      return f;
   if (!strcmp(sym, "glMapBufferRange"))
   {
      *(retro_proc_address_t *)&real_map_range = f;
      return (retro_proc_address_t)count_map_range;
   }
   if (!strcmp(sym, "glReadPixels"))
   {
      *(retro_proc_address_t *)&real_read_pixels = f;
      return (retro_proc_address_t)count_read_pixels;
   }
   return f;
}

static bool environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = log_line;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = getenv("GLF_DIR") ? getenv("GLF_DIR") : "/tmp";
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = gles ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (gles ? cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 && cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION
                  : cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && cb->context_type != RETRO_HW_CONTEXT_OPENGL)
            return false;
         cb->get_current_framebuffer = gl_framebuffer;
         cb->get_proc_address = gl_proc;
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;
         size_t len = strlen(var->key);
         int i;
         for (i = 0; i < option_count; i++)
            if (!strncmp(options[i], var->key, len) && options[i][len] == '=')
            {
               var->value = options[i] + len + 1;
               return true;
            }
         var->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
         return true;
      default:
         return false;
   }
}

static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
static unsigned frame_no;

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
   (void)pitch;
   if (data == RETRO_HW_FRAME_BUFFER_VALID)
   {
      last_w = width;
      last_h = height;
      if (getenv("GLF_EVERY"))
      {
         static unsigned char *row;
         static unsigned row_w;
         uint32_t sum = 2166136261u;
         unsigned y, x;
         if (row_w < width)
         {
            free(row);
            row = malloc((size_t)width * 4);
            row_w = width;
         }
         p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glPixelStorei(GL_PACK_ALIGNMENT, 1);
         for (y = 0; y < height; y++)
         {
            glReadPixels(0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
            for (x = 0; x < width * 4; x++)
               sum = (sum ^ row[x]) * 16777619u;
         }
         printf("frame %u %ux%u %08x\n", frame_no, width, height, sum);
         fflush(stdout);
      }
   }
}
static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_sample_batch(const int16_t *d, size_t frames) { (void)d; return frames; }
static void input_poll(void) { }
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   (void)port; (void)device; (void)index; (void)id;
   return 0;
}

#define SYM(name) do { *(void **)&p_##name = (void *)eglGetProcAddress(#name); if (!p_##name) { fprintf(stderr, "gl_frames: no %s\n", #name); return 1; } } while (0)
#define CORE(name) do { *(void **)&name = dlsym(core, #name); if (!name) { fprintf(stderr, "gl_frames: the core has no %s\n", #name); return 1; } } while (0)

int main(int argc, char **argv)
{
   void (*retro_set_environment)(retro_environment_t);
   void (*retro_set_video_refresh)(retro_video_refresh_t);
   void (*retro_set_audio_sample)(retro_audio_sample_t);
   void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*retro_set_input_poll)(retro_input_poll_t);
   void (*retro_set_input_state)(retro_input_state_t);
   void (*retro_init)(void);
   bool (*retro_load_game)(const struct retro_game_info *);
   void (*retro_get_system_av_info)(struct retro_system_av_info *);
   void (*retro_run)(void);
   void (*retro_unload_game)(void);
   void (*retro_deinit)(void);
   size_t (*retro_serialize_size)(void);
   bool (*retro_serialize)(void *, size_t);
   bool (*retro_unserialize)(const void *, size_t);
   void *state = NULL;
   size_t state_size = 0;
   int ahead_at = getenv("GLF_AHEAD") ? atoi(getenv("GLF_AHEAD")) : -1;
   struct retro_game_info game;
   struct retro_system_av_info av;
   EGLDisplay display;
   EGLContext context;
   EGLConfig config;
   EGLint n;
   void *core;
   int frames, i;
   unsigned char *pixels;
   FILE *out;

   if (argc < 5)
   {
      fprintf(stderr, "usage: gl_frames CORE CONTENT OUT.raw FRAMES [key=value ...]\n");
      return 2;
   }
   gles = getenv("GLF_GLES") != NULL;
   frames = atoi(argv[4]);
   options = argv + 5;
   option_count = argc - 5;

   /* the context: surfaceless, made current with no surface at all */
   {
      PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
      static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES3_BIT, EGL_NONE };
      EGLint ctx_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,
                               EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE };
      display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
      if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)
            || !eglChooseConfig(display, config_attribs, &config, 1, &n) || n < 1)
      {
         fprintf(stderr, "gl_frames: no EGL display (surfaceless)\n");
         return 1;
      }
      eglBindAPI(gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
      if (gles)
         ctx_attribs[4] = EGL_NONE;
      else
      {
         /* compatibility 4.x where there is one, as RetroArch's GL context is */
         ctx_attribs[1] = 4;
         ctx_attribs[3] = 5;
      }
      context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
      if (context == EGL_NO_CONTEXT && !gles)
      {
         ctx_attribs[1] = 3;
         ctx_attribs[3] = 3;
         context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
      }
      if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
      {
         fprintf(stderr, "gl_frames: no OpenGL context\n");
         return 1;
      }
   }
   SYM(glGenFramebuffers);
   SYM(glBindFramebuffer);
   SYM(glFramebufferTexture2D);
   SYM(glGenRenderbuffers);
   SYM(glBindRenderbuffer);
   SYM(glRenderbufferStorage);
   SYM(glFramebufferRenderbuffer);
   SYM(glCheckFramebufferStatus);

   core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!core)
   {
      fprintf(stderr, "gl_frames: %s\n", dlerror());
      return 1;
   }
   CORE(retro_set_environment);
   CORE(retro_set_video_refresh);
   CORE(retro_set_audio_sample);
   CORE(retro_set_audio_sample_batch);
   CORE(retro_set_input_poll);
   CORE(retro_set_input_state);
   CORE(retro_init);
   CORE(retro_load_game);
   CORE(retro_get_system_av_info);
   CORE(retro_run);
   CORE(retro_serialize_size);
   CORE(retro_serialize);
   CORE(retro_unserialize);
   CORE(retro_unload_game);
   CORE(retro_deinit);

   retro_set_environment(environment);
   retro_set_video_refresh(video_refresh);
   retro_set_audio_sample(audio_sample);
   retro_set_audio_sample_batch(audio_sample_batch);
   retro_set_input_poll(input_poll);
   retro_set_input_state(input_state);
   retro_init();
   memset(&game, 0, sizeof(game));
   game.path = argv[2];
   if (!retro_load_game(&game) || !have_hw)
   {
      fprintf(stderr, "gl_frames: the game did not load%s\n", have_hw ? "" : " (no OpenGL asked for)");
      return 1;
   }
   retro_get_system_av_info(&av);
   fb_w = av.geometry.max_width;
   fb_h = av.geometry.max_height;

   /* the framebuffer the core draws in, of the largest size it says */
   glGenTextures(1, &color);
   glBindTexture(GL_TEXTURE_2D, color);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb_w, fb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glBindTexture(GL_TEXTURE_2D, 0);
   p_glGenFramebuffers(1, &fbo);
   p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
   if (hw.depth)
   {
      p_glGenRenderbuffers(1, &depth);
      p_glBindRenderbuffer(GL_RENDERBUFFER, depth);
      p_glRenderbufferStorage(GL_RENDERBUFFER, hw.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, fb_w, fb_h);
      p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, hw.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
            GL_RENDERBUFFER, depth);
   }
   if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
   {
      fprintf(stderr, "gl_frames: the framebuffer is not complete\n");
      return 1;
   }
   if (hw.context_reset)
      hw.context_reset();

   for (i = 0; i < frames; i++)
   {
      frame_no = i;
#ifdef ALLOC_COUNT
      if (getenv("GLF_ALLOCS") && i == atoi(getenv("GLF_ALLOCS")))
      {
         alloc_count_kinds(GLF_A_KINDS, glf_a_names, glf_a_site);
         alloc_count_start((void *)retro_run);
      }
#endif
      in_run = 1;
      retro_run();
      in_run = 0;
      if (ahead_at >= 0 && i >= ahead_at)
      {
         /* the state after this frame; the frame after it, shown; back to the state */
         if (!state)
         {
            state_size = retro_serialize_size();
            state = malloc(state_size);
         }
         if (!state || !retro_serialize(state, state_size))
         {
            fprintf(stderr, "gl_frames: the state could not be saved (frame %d)\n", i);
            return 1;
         }
         retro_run();
         if (!retro_unserialize(state, state_size))
         {
            fprintf(stderr, "gl_frames: the state could not be loaded (frame %d)\n", i);
            return 1;
         }
      }
   }

#ifdef ALLOC_COUNT
   if (getenv("GLF_ALLOCS"))
   {
      char what[64];
      alloc_count_stop();
      snprintf(what, sizeof(what), "frames %d to %d", atoi(getenv("GLF_ALLOCS")), frames - 1);
      alloc_count_print("gl_frames", what, getenv("GLF_ALLOC_SITES") != NULL);
   }
#endif
   if (last_w == 0 || last_h == 0)
   {
      fprintf(stderr, "gl_frames: no frame was drawn\n");
      /* (the core let go all the same, as a frontend does) */
      if (hw.context_destroy)
         hw.context_destroy();
      retro_unload_game();
      retro_deinit();
      return 1;
   }
   pixels = malloc((size_t)last_w * last_h * 4);
   p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, last_w, last_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   out = fopen(argv[3], "wb");
   if (!out)
   {
      perror(argv[3]);
      return 1;
   }
   {
      const uint32_t header[4] = { 0x52484b56u /* "VKHR" */, 37, last_w, last_h };
      fwrite(header, sizeof(header), 1, out);
      /* top first: OpenGL's rows are bottom first unless the core says otherwise */
      for (i = 0; i < (int)last_h; i++)
      {
         const unsigned row = hw.bottom_left_origin ? last_h - 1 - i : (unsigned)i;
         fwrite(pixels + (size_t)row * last_w * 4, last_w * 4, 1, out);
      }
   }
   fclose(out);
   printf("gl_frames: %d frames, picture %ux%u\n", frames, last_w, last_h);

   if (hw.context_destroy)
      hw.context_destroy();
   retro_unload_game();
   retro_deinit();
   free(pixels);
   free(state);
   if (getenv("GLF_READS"))
      printf("reads %ld\n", reads);
   if (getenv("GLF_COUNT_PROGRAMS"))
      printf("programs: %ld compiled, %ld linked, %ld loaded in retro_run\n", compiled, linked, loaded);
   return 0;
}
