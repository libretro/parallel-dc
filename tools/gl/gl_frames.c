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
 * which a run's last frame seldom is).
 *
 * Built with: cc -o gl_frames gl_frames.c -lEGL -lGL -ldl */
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
static retro_proc_address_t gl_proc(const char *sym) { return (retro_proc_address_t)eglGetProcAddress(sym); }

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
      retro_run();
   }

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
   return 0;
}
