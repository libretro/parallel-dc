/* A libretro frontend that gives the core a Direct3D 9 device (as
 * RetroArch's d3d9 driver does: retro_hw_render_interface_d3d9, a device of
 * its own on a hidden window) and checksums every frame the core hands
 * over, "frame N WxH SUM" a line, as tools/gl/gl_frames.c does with
 * GLF_EVERY - so that two builds' runs can be compared frame for frame
 * (tools/rend/d3d_same.sh). The last frame is written out as vk_hdr.c
 * writes its pictures. Built with mingw-w64 and run under wine.
 *
 *   d3d9_frames.exe CORE.dll CONTENT OUT.raw FRAMES [key=value ...]
 *
 * key=value pairs are core options; D3D9F_DIR is the system/save directory
 * (the current one by default). C89. */
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <libretro_d3d9.h>

#define VKH_MAGIC 0x52484b56u
#define VKH_RGBA8 37u

static char *opt_keys[64], *opt_vals[64];
static int nopts;
static char dir[MAX_PATH] = ".";
static struct retro_hw_render_callback hw;
static int have_hw;

static IDirect3DDevice9 *device;
static IDirect3DTexture9 *frame;   /* the last one handed over, a reference of ours */
static struct retro_hw_render_interface_d3d9 iface;
static CRITICAL_SECTION device_lock;   /* (recursive, as the interface's) */
static int frontend_used = 1;
static unsigned frame_no, frame_w, frame_h;
static unsigned char *pixels;          /* the last frame, BGRA */
static size_t pixels_size;

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("D3D9F_VERBOSE"))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static bool lock_context(void *handle)
{
   bool used;
   (void)handle;
   EnterCriticalSection(&device_lock);
   used = frontend_used != 0;
   frontend_used = 0;
   return used;
}

static void unlock_context(void *handle)
{
   (void)handle;
   LeaveCriticalSection(&device_lock);
}

static void set_texture(void *handle, LPDIRECT3DTEXTURE9 texture)
{
   (void)handle;
   if (texture)
      IDirect3DTexture9_AddRef(texture);
   if (frame)
      IDirect3DTexture9_Release(frame);
   frame = texture;
}

static unsigned get_sync_index(void *handle) { (void)handle; return 0; }
static unsigned get_sync_index_mask(void *handle) { (void)handle; return 1; }
static void wait_sync_index(void *handle) { (void)handle; }

static bool environ_cb(unsigned cmd, void *data)
{
   int i;
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
         *(const char**)data = dir;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(const enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_XRGB8888
               || *(const enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_RGB565;
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool*)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         for (i = 0; i < nopts; i++)
            if (!strcmp(opt_keys[i], var->key))
            {
               var->value = opt_vals[i];
               return true;
            }
         var->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned*)data = RETRO_HW_CONTEXT_D3D9;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         const struct retro_hw_render_callback *cb = (const struct retro_hw_render_callback*)data;
         if (cb->context_type != RETRO_HW_CONTEXT_D3D9)
            return false;
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         *(const struct retro_hw_render_interface**)data = (const struct retro_hw_render_interface*)&iface;
         return true;
      default:
         return false;
   }
}

/* The frame handed over, read back: its rows to pixels, and its sum */
static void video_cb(const void *d, unsigned w, unsigned h, size_t p)
{
   IDirect3DSurface9 *level = NULL, *copy = NULL;
   D3DSURFACE_DESC desc;
   D3DLOCKED_RECT rect;
   unsigned y, x, sum = 2166136261u;
   (void)p;
   frame_no++;
   if (d != RETRO_HW_FRAME_BUFFER_VALID || frame == NULL)
      return;
   EnterCriticalSection(&device_lock);
   frontend_used = 1;
   if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(frame, 0, &level))
         && SUCCEEDED(IDirect3DSurface9_GetDesc(level, &desc))
         && SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height,
               desc.Format, D3DPOOL_SYSTEMMEM, &copy, NULL))
         && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(device, level, copy))
         && SUCCEEDED(IDirect3DSurface9_LockRect(copy, &rect, NULL, D3DLOCK_READONLY)))
   {
      /* (the picture is the top left w x h of the texture) */
      if (w > desc.Width)
         w = desc.Width;
      if (h > desc.Height)
         h = desc.Height;
      if ((size_t)w * h * 4 > pixels_size)
      {
         free(pixels);
         pixels_size = (size_t)w * h * 4;
         pixels = (unsigned char*)malloc(pixels_size);
      }
      for (y = 0; y < h && pixels; y++)
      {
         const unsigned char *row = (const unsigned char*)rect.pBits + (size_t)y * rect.Pitch;
         memcpy(pixels + (size_t)y * w * 4, row, (size_t)w * 4);
         /* (the colour only: what is in the unused byte is no one's) */
         for (x = 0; x < w * 4; x++)
            if ((x & 3) != 3)
               sum = (sum ^ row[x]) * 16777619u;
      }
      IDirect3DSurface9_UnlockRect(copy);
      frame_w = w;
      frame_h = h;
      printf("frame %u %ux%u %08x\n", frame_no, w, h, sum);
   }
   else
      printf("frame %u: not read back\n", frame_no);
   if (copy)
      IDirect3DSurface9_Release(copy);
   if (level)
      IDirect3DSurface9_Release(level);
   LeaveCriticalSection(&device_lock);
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

static int write_raw(const char *path)
{
   unsigned int header[4];
   unsigned y, x;
   unsigned char *line;
   FILE *f = fopen(path, "wb");
   if (!f || !pixels)
      return 0;
   header[0] = VKH_MAGIC;
   header[1] = VKH_RGBA8;
   header[2] = frame_w;
   header[3] = frame_h;
   fwrite(header, sizeof(header), 1, f);
   line = (unsigned char*)malloc((size_t)frame_w * 4);
   for (y = 0; y < frame_h && line; y++)
   {
      memcpy(line, pixels + (size_t)y * frame_w * 4, (size_t)frame_w * 4);
      for (x = 0; x < frame_w; x++)
      {
         unsigned char t = line[x * 4];
         line[x * 4] = line[x * 4 + 2];
         line[x * 4 + 2] = t;
         line[x * 4 + 3] = 255;
      }
      fwrite(line, 1, (size_t)frame_w * 4, f);
   }
   free(line);
   fclose(f);
   return 1;
}

typedef void (*set_env_t)(retro_environment_t);
typedef void (*set_video_t)(retro_video_refresh_t);
typedef void (*set_audio_t)(retro_audio_sample_t);
typedef void (*set_audio_batch_t)(retro_audio_sample_batch_t);
typedef void (*set_poll_t)(retro_input_poll_t);
typedef void (*set_state_t)(retro_input_state_t);
typedef void (*init_t)(void);
typedef bool (*load_game_t)(const struct retro_game_info*);
typedef void (*run_t)(void);
typedef void (*deinit_t)(void);
typedef void (*unload_t)(void);

int main(int argc, char **argv)
{
   HMODULE core, compiler;
   WNDCLASSA wc;
   HWND window;
   IDirect3D9 *d3d;
   D3DPRESENT_PARAMETERS pp;
   struct retro_game_info game;
   unsigned frames, n;
   int i;
   set_env_t retro_set_environment;
   set_video_t retro_set_video_refresh;
   set_audio_t retro_set_audio_sample;
   set_audio_batch_t retro_set_audio_sample_batch;
   set_poll_t retro_set_input_poll;
   set_state_t retro_set_input_state;
   init_t retro_init;
   load_game_t retro_load_game;
   run_t retro_run;
   unload_t retro_unload_game;
   deinit_t retro_deinit;

   if (argc < 5)
   {
      fprintf(stderr, "usage: d3d9_frames core.dll content out.raw frames [key=value ...]\n");
      return 2;
   }
   if (getenv("D3D9F_DIR"))
      strncpy(dir, getenv("D3D9F_DIR"), sizeof(dir) - 1);
   frames = (unsigned)atoi(argv[4]);
   for (i = 5; i < argc && nopts < 64; i++)
   {
      char *eq = strchr(argv[i], '=');
      if (!eq)
         continue;
      *eq = '\0';
      opt_keys[nopts] = argv[i];
      opt_vals[nopts++] = eq + 1;
   }
   setvbuf(stdout, NULL, _IOLBF, 0);
   InitializeCriticalSection(&device_lock);

   /* the device first, on a window no one sees, as the frontend has it before it loads the content */
   memset(&wc, 0, sizeof(wc));
   wc.lpfnWndProc = DefWindowProcA;
   wc.hInstance = GetModuleHandleA(NULL);
   wc.lpszClassName = "d3d9_frames";
   RegisterClassA(&wc);
   window = CreateWindowA("d3d9_frames", "d3d9_frames", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
         NULL, NULL, wc.hInstance, NULL);
   d3d = Direct3DCreate9(D3D_SDK_VERSION);
   if (!window || !d3d)
   {
      fprintf(stderr, "d3d9_frames: no window or no Direct3D 9\n");
      return 2;
   }
   memset(&pp, 0, sizeof(pp));
   pp.Windowed = TRUE;
   pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
   pp.BackBufferFormat = D3DFMT_X8R8G8B8;
   pp.BackBufferWidth = 640;
   pp.BackBufferHeight = 480;
   pp.hDeviceWindow = window;
   if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &pp, &device)))
   {
      fprintf(stderr, "d3d9_frames: no Direct3D 9 device\n");
      return 2;
   }
   compiler = LoadLibraryA("d3dcompiler_47.dll");
   iface.interface_type = RETRO_HW_RENDER_INTERFACE_D3D9;
   iface.interface_version = RETRO_HW_RENDER_INTERFACE_D3D9_VERSION;
   iface.handle = NULL;
   iface.device = device;
   iface.D3DCompile = compiler ? (retro_d3d9_compile_t)(void (*)(void))GetProcAddress(compiler, "D3DCompile") : NULL;
   iface.lock_context = lock_context;
   iface.unlock_context = unlock_context;
   iface.set_texture = set_texture;
   iface.get_sync_index = get_sync_index;
   iface.get_sync_index_mask = get_sync_index_mask;
   iface.wait_sync_index = wait_sync_index;

   core = LoadLibraryA(argv[1]);
   if (!core)
   {
      fprintf(stderr, "d3d9_frames: cannot load %s\n", argv[1]);
      return 2;
   }
#define CORE_FN(type, name) name = (type)(void (*)(void))GetProcAddress(core, #name)
   CORE_FN(set_env_t, retro_set_environment);
   CORE_FN(set_video_t, retro_set_video_refresh);
   CORE_FN(set_audio_t, retro_set_audio_sample);
   CORE_FN(set_audio_batch_t, retro_set_audio_sample_batch);
   CORE_FN(set_poll_t, retro_set_input_poll);
   CORE_FN(set_state_t, retro_set_input_state);
   CORE_FN(init_t, retro_init);
   CORE_FN(load_game_t, retro_load_game);
   CORE_FN(run_t, retro_run);
   CORE_FN(unload_t, retro_unload_game);
   CORE_FN(deinit_t, retro_deinit);
   retro_set_environment(environ_cb);
   retro_init();
   retro_set_video_refresh(video_cb);
   retro_set_audio_sample(audio_cb);
   retro_set_audio_sample_batch(audio_batch_cb);
   retro_set_input_poll(input_poll_cb);
   retro_set_input_state(input_state_cb);
   memset(&game, 0, sizeof(game));
   game.path = argv[2];
   if (!retro_load_game(&game))
   {
      fprintf(stderr, "d3d9_frames: retro_load_game failed\n");
      return 1;
   }
   if (!have_hw)
   {
      fprintf(stderr, "d3d9_frames: the core did not ask for Direct3D 9\n");
      return 1;
   }
   hw.context_reset();
   for (n = 0; n < frames; n++)
      retro_run();
   if (!write_raw(argv[3]))
   {
      fprintf(stderr, "d3d9_frames: no picture was handed over\n");
      return 1;
   }
   hw.context_destroy();
   set_texture(NULL, NULL);
   retro_unload_game();
   retro_deinit();
   IDirect3DDevice9_Release(device);
   IDirect3D9_Release(d3d);
   printf("d3d9_frames: %u frames, the last %ux%u\n", frames, frame_w, frame_h);
   fflush(stdout);
   /* (as the other frontends: no handler of the core's at exit) */
   ExitProcess(0);
   return 0;
}
