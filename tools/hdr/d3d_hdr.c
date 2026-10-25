/* A frontend for the core's HDR output (Color Format) with Direct3D 11
 * and 12, the core's Windows build run under wine: it runs the core's
 * frames as RetroArch does with version 1 of each interface, answers the
 * HDR questions as vk_hdr.c does, and writes the last picture handed over
 * in vk_hdr's form, for vk_hdr --compare and --same to judge as they do
 * the Vulkan renderer's. d3d_hdr.sh.
 *
 * With D3DH_REFUSED=1 the interface has no device (Direct3D 11) or no
 * queue (12), as when a frontend's device was not to be had: the core is
 * to run its frames, drawing none, and not take the frontend down.
 *
 * With D3DH_UPSCALER=OWxOH:RWxRH (Direct3D 12) the frontend has a temporal
 * upscaler (rend/upscaler.h) that asks for frames of RWxRH for a screen
 * of OWxOH: the core is to draw them at that size and hand over each
 * frame's motion vectors and depth, which are read back and looked at as
 * vk_hdr.c looks at the Vulkan renderers'.
 *
 *   D3DH_API=11|12 D3DH_FRONTEND=sdr|hdr10|scrgb D3DH_DIR=dir \
 *      d3d_hdr core.dll content out.raw frames [key=value ...]
 *
 * C89. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <libretro_d3d11.h>
#include <libretro_d3d12.h>

/* vk_hdr's picture: its header's format is the Vulkan format of the pixels */
#define VKH_MAGIC          0x52484b56u
#define VKH_RGBA8          37u   /* VK_FORMAT_R8G8B8A8_UNORM */
#define VKH_A2B10G10R10    64u   /* VK_FORMAT_A2B10G10R10_UNORM_PACK32: DXGI's R10G10B10A2 */
#define VKH_RGBA16F        97u   /* VK_FORMAT_R16G16B16A16_SFLOAT */

static char *opt_keys[64], *opt_vals[64];
static int nopts;
static int frontend_hdr;    /* 0 sdr, 1 hdr10, 2 scrgb */
static int api;
static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_hw_render_callback hw;
static int have_hw;
static unsigned frame_w, frame_h;
static char dir[MAX_PATH];

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("D3DH_VERBOSE"))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static void answer_hdr(struct retro_hdr_metadata *h)
{
   h->output_mode      = frontend_hdr == 2 ? 2 : 1;
   h->source_formats   = RETRO_HDR_SOURCE_FORMAT_HDR10_2101010
                       | (frontend_hdr == 2 ? RETRO_HDR_SOURCE_FORMAT_SCRGB_RGBA16F : 0);
   h->paper_white_nits = 200.0f;
   h->max_nits         = 1000.0f;
   h->passthrough      = true;
}

/* ---- Direct3D 11, version 1: the frame is what is bound at pixel shader slot 0 ---- */

static ID3D11Device *d11;
static ID3D11DeviceContext *c11;
static ID3D11ShaderResourceView *frame11;
static struct retro_hw_render_interface_d3d11 iface11;

/* ---- Direct3D 12, version 1: the frame is handed by set_texture ---- */

static ID3D12Device *d12;
static ID3D12CommandQueue *q12;
static ID3D12Resource *frame12;
static DXGI_FORMAT frame12_format;
static struct retro_hw_render_interface_d3d12 iface12;

static void set_texture12(void *handle, ID3D12Resource *texture, DXGI_FORMAT format)
{
   (void)handle;
   if (texture)
      ID3D12Resource_AddRef(texture);
   if (frame12)
      ID3D12Resource_Release(frame12);
   frame12        = texture;
   frame12_format = format;
}

/* ---- the temporal upscaler: the sizes D3DH_UPSCALER names, and what the core handed over ---- */

static unsigned up_out_w, up_out_h, up_render_w, up_render_h;
static struct retro_hw_upscaler_inputs up_inputs;
static unsigned up_count, up_bad;
static int up_first_reset;

static bool up_get_request(void *handle, struct retro_hw_upscaler_request *r)
{
   (void)handle;
   memset(r, 0, sizeof(*r));
   r->output_width      = up_out_w;
   r->output_height     = up_out_h;
   r->render_width      = up_render_w;
   r->render_height     = up_render_h;
   r->min_render_width  = up_render_w / 2;
   r->min_render_height = up_render_h / 2;
   r->max_render_width  = up_out_w;
   r->max_render_height = up_out_h;
   r->mip_bias          = -1.0f;
   return true;
}

static void up_set_inputs(void *handle, const struct retro_hw_upscaler_inputs *inputs)
{
   int bad = 0;
   (void)handle;
   up_inputs = *inputs;
   /* (an image is the resource, in a state the frontend transitions from with one barrier) */
   if (!inputs->motion_vectors.image || !inputs->depth.image)
      bad = 1;
   if (inputs->motion_vectors.width != up_render_w || inputs->motion_vectors.height != up_render_h
         || inputs->depth.width != up_render_w || inputs->depth.height != up_render_h)
      bad = 1;
   if (inputs->motion_vectors.format != DXGI_FORMAT_R16G16_FLOAT || inputs->depth.format != DXGI_FORMAT_R32_FLOAT)
      bad = 1;
   if (!(inputs->motion_vectors.state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
         || !(inputs->depth.state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
      bad = 1;
   if (inputs->jitter_x < -0.5f || inputs->jitter_x > 0.5f || inputs->jitter_y < -0.5f || inputs->jitter_y > 0.5f)
      bad = 1;
   if (!(inputs->flags & RETRO_HW_UPSCALER_DEPTH_INVERTED) || (inputs->flags & RETRO_HW_UPSCALER_MV_OUTPUT_RES))
      bad = 1;
   if (up_count == 0)
      up_first_reset = (inputs->flags & RETRO_HW_UPSCALER_RESET) != 0;
   if (bad)
   {
      fprintf(stderr, "d3d_hdr: upscaler inputs %u: mv %ux%u format %u state %x, depth %ux%u format %u state %x, jitter %f %f, flags %x\n",
            up_count, inputs->motion_vectors.width, inputs->motion_vectors.height, inputs->motion_vectors.format, inputs->motion_vectors.state,
            inputs->depth.width, inputs->depth.height, inputs->depth.format, inputs->depth.state, inputs->jitter_x, inputs->jitter_y, inputs->flags);
      up_bad++;
   }
   up_count++;
}

static const struct retro_hw_render_upscaler_interface up_iface = {
   RETRO_HW_RENDER_UPSCALER_INTERFACE_VERSION, NULL, up_get_request, up_set_inputs
};

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
      {
         enum retro_pixel_format f = *(const enum retro_pixel_format*)data;
         if (f == RETRO_PIXEL_FORMAT_HDR10_2101010 && frontend_hdr == 0)
            return false;
         if (f == RETRO_PIXEL_FORMAT_SCRGB_RGBA16F && frontend_hdr != 2)
            return false;
         pix_fmt = f;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HDR_METADATA:
      {
         struct retro_hdr_metadata *h = (struct retro_hdr_metadata*)data;
         if (frontend_hdr == 0 || h->version != 1 || h->size < sizeof(*h))
            return false;
         answer_hdr(h);
         return true;
      }
      case RETRO_ENVIRONMENT_GET_DISPLAY_METADATA:
      {
         struct retro_display_metadata *d = (struct retro_display_metadata*)data;
         if (frontend_hdr == 0 || d->version != 1 || d->size < sizeof(*d))
            return false;
         d->hdr.version = 1;
         d->hdr.size    = sizeof(d->hdr);
         answer_hdr(&d->hdr);
         return true;
      }
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
         *(unsigned*)data = api == 12 ? RETRO_HW_CONTEXT_D3D12 : RETRO_HW_CONTEXT_D3D11;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         const struct retro_hw_render_callback *cb = (const struct retro_hw_render_callback*)data;
         if (cb->context_type != (api == 12 ? RETRO_HW_CONTEXT_D3D12 : RETRO_HW_CONTEXT_D3D11))
            return false;
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         *(const struct retro_hw_render_interface**)data = api == 12
               ? (const struct retro_hw_render_interface*)&iface12
               : (const struct retro_hw_render_interface*)&iface11;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
         /* (version 1 of the interfaces: no negotiation) */
         ((struct retro_hw_render_context_negotiation_interface*)data)->interface_version = 0;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_UPSCALER_INTERFACE:
         if (!up_render_w || api != 12)
            return false;
         *(const struct retro_hw_render_upscaler_interface**)data = &up_iface;
         return true;
      default:
         return false;
   }
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t p)
{
   (void)p;
   if (d != RETRO_HW_FRAME_BUFFER_VALID)
      return;
   frame_w = w;
   frame_h = h;
   if (api == 11)
   {
      ID3D11ShaderResourceView *v = NULL;
      ID3D11DeviceContext_PSGetShaderResources(c11, 0, 1, &v);
      if (frame11)
         ID3D11ShaderResourceView_Release(frame11);
      frame11 = v;
   }
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

/* ---- the picture back ---- */

static unsigned vkh_format(DXGI_FORMAT f)
{
   switch (f)
   {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
         return VKH_RGBA8;
      case DXGI_FORMAT_R10G10B10A2_UNORM:
         return VKH_A2B10G10R10;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
         return VKH_RGBA16F;
      default:
         return 0;
   }
}

/* @rows (@pitch apart) of @format, frame_w x frame_h of them, to @path in vk_hdr's form */
static int write_raw(const char *path, const unsigned char *rows, size_t pitch, DXGI_FORMAT format)
{
   const unsigned vf = vkh_format(format), bpp = vf == VKH_RGBA16F ? 8 : 4;
   const int bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM;
   unsigned y, x;
   unsigned int header[4];
   unsigned char *line;
   FILE *f;
   if (!vf)
   {
      fprintf(stderr, "d3d_hdr: the picture is DXGI format %d\n", (int)format);
      return 0;
   }
   f = fopen(path, "wb");
   if (!f)
      return 0;
   header[0] = VKH_MAGIC;
   header[1] = vf;
   header[2] = frame_w;
   header[3] = frame_h;
   fwrite(header, sizeof(header), 1, f);
   line = (unsigned char*)malloc((size_t)frame_w * bpp);
   for (y = 0; y < frame_h; y++)
   {
      memcpy(line, rows + y * pitch, (size_t)frame_w * bpp);
      if (bgra)
         for (x = 0; x < frame_w; x++)
         {
            unsigned char t = line[x * 4];
            line[x * 4] = line[x * 4 + 2];
            line[x * 4 + 2] = t;
         }
      fwrite(line, 1, (size_t)frame_w * bpp, f);
   }
   free(line);
   fclose(f);
   return 1;
}

static int read_back11(const char *path, DXGI_FORMAT *format)
{
   ID3D11Resource *res;
   ID3D11Texture2D *staging;
   D3D11_TEXTURE2D_DESC td;
   D3D11_MAPPED_SUBRESOURCE map;
   int ok;
   if (!frame11)
      return 0;
   ID3D11ShaderResourceView_GetResource(frame11, &res);
   ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res, &td);
   *format = td.Format;
   td.Usage = D3D11_USAGE_STAGING;
   td.BindFlags = 0;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   td.MiscFlags = 0;
   if (td.Width < frame_w || td.Height < frame_h
         || FAILED(ID3D11Device_CreateTexture2D(d11, &td, NULL, &staging)))
      return 0;
   ID3D11DeviceContext_CopyResource(c11, (ID3D11Resource*)staging, res);
   if (FAILED(ID3D11DeviceContext_Map(c11, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &map)))
      return 0;
   ok = write_raw(path, (const unsigned char*)map.pData, map.RowPitch, td.Format);
   ID3D11DeviceContext_Unmap(c11, (ID3D11Resource*)staging, 0);
   ID3D11Texture2D_Release(staging);
   ID3D11Resource_Release(res);
   return ok;
}

/* @texture, in @state, copied whole to a readback buffer on the queue,
 * waited for and mapped: its rows, @pitch apart, at least @w x @h of
 * them; NULL if not. (Never unmapped: a run reads back a few.) */
static const unsigned char *copy_out12(ID3D12Resource *texture, D3D12_RESOURCE_STATES state, unsigned w, unsigned h, UINT *pitch)
{
   D3D12_RESOURCE_DESC rd = { 0 }, bd;
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
   D3D12_HEAP_PROPERTIES hp;
   D3D12_TEXTURE_COPY_LOCATION src, dst;
   D3D12_RESOURCE_BARRIER b[2];
   ID3D12Resource *readback;
   ID3D12CommandAllocator *alloc;
   ID3D12GraphicsCommandList *list;
   ID3D12CommandList *l;
   ID3D12Fence *fence;
   HANDLE ev;
   UINT64 total;
   void *map;
   if (!texture)
      return NULL;
   /* (a struct returned: mingw-w64's C binding takes where it goes) */
   texture->lpVtbl->GetDesc(texture, &rd);
   ID3D12Device_GetCopyableFootprints(d12, &rd, 0, 1, 0, &fp, NULL, NULL, &total);
   memset(&hp, 0, sizeof(hp));
   hp.Type = D3D12_HEAP_TYPE_READBACK;
   memset(&bd, 0, sizeof(bd));
   bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   bd.Width = total;
   bd.Height = 1;
   bd.DepthOrArraySize = 1;
   bd.MipLevels = 1;
   bd.SampleDesc.Count = 1;
   bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   if (FAILED(ID3D12Device_CreateCommittedResource(d12, &hp, D3D12_HEAP_FLAG_NONE, &bd,
         D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&readback))
         || FAILED(ID3D12Device_CreateCommandAllocator(d12, D3D12_COMMAND_LIST_TYPE_DIRECT,
               &IID_ID3D12CommandAllocator, (void**)&alloc))
         || FAILED(ID3D12Device_CreateCommandList(d12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
               &IID_ID3D12GraphicsCommandList, (void**)&list))
         || FAILED(ID3D12Device_CreateFence(d12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence)))
      return NULL;
   memset(b, 0, sizeof(b));
   b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b[0].Transition.pResource = texture;
   b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b[0].Transition.StateBefore = state;
   b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
   b[1] = b[0];
   b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
   b[1].Transition.StateAfter = state;
   if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
      ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b[0]);
   memset(&src, 0, sizeof(src));
   memset(&dst, 0, sizeof(dst));
   src.pResource = texture;
   src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   dst.pResource = readback;
   dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   dst.PlacedFootprint = fp;
   ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
   if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
      ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b[1]);
   ID3D12GraphicsCommandList_Close(list);
   l = (ID3D12CommandList*)list;
   ID3D12CommandQueue_ExecuteCommandLists(q12, 1, &l);
   ID3D12CommandQueue_Signal(q12, fence, 1);
   ev = CreateEventA(NULL, FALSE, FALSE, NULL);
   ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
   WaitForSingleObject(ev, INFINITE);
   if (rd.Width < w || rd.Height < h || FAILED(ID3D12Resource_Map(readback, 0, NULL, &map)))
      return NULL;
   *pitch = fp.Footprint.RowPitch;
   return (const unsigned char*)map;
}

static int read_back12(const char *path, DXGI_FORMAT *format)
{
   UINT pitch;
   const unsigned char *rows = copy_out12(frame12, iface12.required_state, frame_w, frame_h, &pitch);
   *format = frame12_format;
   if (!rows)
      return 0;
   return write_raw(path, rows, pitch, frame12_format);
}

/* ---- the upscaler's inputs back ---- */

static float half_to_float(unsigned short h)
{
   const unsigned s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
   float v;
   if (e == 0)
      v = (float)m / 1024.0f / 16384.0f;
   else if (e == 31)
      v = m ? (float)(0.0 / 0.0) : (float)HUGE_VAL;
   else
      v = (1.0f + (float)m / 1024.0f) * (float)pow(2.0, (double)e - 15.0);
   return s ? -v : v;
}

/* The motion vectors checked: finite and under a pixel everywhere, the
 * scene standing still; the depth with something in it: as vk_hdr.c */
static int check_upscaler_inputs12(void)
{
   UINT pitch;
   const unsigned char *rows;
   unsigned x, y, n = up_render_w * up_render_h, drawn = 0, moving = 0, nonfinite = 0;

   rows = copy_out12((ID3D12Resource*)(uintptr_t)up_inputs.motion_vectors.image,
         (D3D12_RESOURCE_STATES)up_inputs.motion_vectors.state, up_render_w, up_render_h, &pitch);
   if (!rows)
   {
      fprintf(stderr, "d3d_hdr: the motion vectors could not be read back\n");
      return 0;
   }
   for (y = 0; y < up_render_h; y++)
      for (x = 0; x < up_render_w * 2; x++)
      {
         unsigned short h;
         float v;
         memcpy(&h, rows + (size_t)y * pitch + (size_t)x * 2, 2);
         v = half_to_float(h);
         if (v != v || fabsf(v) == (float)HUGE_VAL)
            nonfinite++;
         else if (fabsf(v) > 1.0f)
            moving++;
      }
   if (nonfinite || moving)
   {
      fprintf(stderr, "d3d_hdr: motion vectors: %u not finite, %u over a pixel, in a scene standing still\n", nonfinite, moving);
      return 0;
   }
   rows = copy_out12((ID3D12Resource*)(uintptr_t)up_inputs.depth.image,
         (D3D12_RESOURCE_STATES)up_inputs.depth.state, up_render_w, up_render_h, &pitch);
   if (!rows)
   {
      fprintf(stderr, "d3d_hdr: the depth could not be read back\n");
      return 0;
   }
   for (y = 0; y < up_render_h; y++)
      for (x = 0; x < up_render_w; x++)
      {
         unsigned d;
         memcpy(&d, rows + (size_t)y * pitch + (size_t)x * 4, 4);
         if (d != 0)
            drawn++;
      }
   if (drawn < n / 8)
   {
      fprintf(stderr, "d3d_hdr: the depth has %u of %u pixels drawn\n", drawn, n);
      return 0;
   }
   return 1;
}

/* ---- running the core ---- */

typedef void (RETRO_CALLCONV *set_env_t)(retro_environment_t);
typedef void (RETRO_CALLCONV *set_video_t)(retro_video_refresh_t);
typedef void (RETRO_CALLCONV *set_audio_t)(retro_audio_sample_t);
typedef void (RETRO_CALLCONV *set_audio_batch_t)(retro_audio_sample_batch_t);
typedef void (RETRO_CALLCONV *set_poll_t)(retro_input_poll_t);
typedef void (RETRO_CALLCONV *set_state_t)(retro_input_state_t);
typedef void (RETRO_CALLCONV *init_t)(void);
typedef void (RETRO_CALLCONV *run_t)(void);
typedef bool (RETRO_CALLCONV *load_game_t)(const struct retro_game_info*);

static int finish(int code)
{
   fflush(stdout);
   fflush(stderr);
   /* (the core is not unloaded: as vk_hdr, the run is what is judged) */
   ExitProcess((UINT)code);
   return code;
}

int main(int argc, char **argv)
{
   HMODULE core;
   struct retro_game_info game;
   const char *fe = getenv("D3DH_FRONTEND");
   unsigned frames, n, i;
   DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, expect;
   set_env_t retro_set_environment;
   set_video_t retro_set_video_refresh;
   set_audio_t retro_set_audio_sample;
   set_audio_batch_t retro_set_audio_sample_batch;
   set_poll_t retro_set_input_poll;
   set_state_t retro_set_input_state;
   init_t retro_init;
   load_game_t retro_load_game;
   run_t retro_run;
   int ok;

   if (argc < 5)
   {
      fprintf(stderr, "usage: d3d_hdr core.dll content out.raw frames [key=value ...]\n");
      return 2;
   }
   api = getenv("D3DH_API") && !strcmp(getenv("D3DH_API"), "12") ? 12 : 11;
   frontend_hdr = fe && !strcmp(fe, "scrgb") ? 2 : fe && !strcmp(fe, "hdr10") ? 1 : 0;
   if (getenv("D3DH_UPSCALER")
         && sscanf(getenv("D3DH_UPSCALER"), "%ux%u:%ux%u", &up_out_w, &up_out_h, &up_render_w, &up_render_h) != 4)
   {
      fprintf(stderr, "d3d_hdr: D3DH_UPSCALER is OWxOH:RWxRH\n");
      return 2;
   }
   strncpy(dir, getenv("D3DH_DIR") ? getenv("D3DH_DIR") : ".", sizeof(dir) - 1);
   frames = (unsigned)atoi(argv[4]);
   for (i = 5; i < (unsigned)argc && nopts < 64; i++)
   {
      char *eq = strchr(argv[i], '=');
      if (!eq)
         continue;
      *eq = '\0';
      opt_keys[nopts] = argv[i];
      opt_vals[nopts++] = eq + 1;
   }

   /* the device first: the frontend has it before it loads the content */
   if (api == 11)
   {
      D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
      if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION,
            &d11, &got, &c11)))
      {
         fprintf(stderr, "d3d_hdr: no Direct3D 11 device\n");
         return 2;
      }
      iface11.interface_type    = RETRO_HW_RENDER_INTERFACE_D3D11;
      iface11.interface_version = RETRO_HW_RENDER_INTERFACE_D3D11_VERSION;
      iface11.device            = d11;
      iface11.context           = c11;
      iface11.featureLevel      = got;
      iface11.D3DCompile        = (pD3DCompile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
   }
   else
   {
      D3D12_COMMAND_QUEUE_DESC qd;
      if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&d12)))
      {
         fprintf(stderr, "d3d_hdr: no Direct3D 12 device\n");
         return 2;
      }
      memset(&qd, 0, sizeof(qd));
      qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      if (FAILED(ID3D12Device_CreateCommandQueue(d12, &qd, &IID_ID3D12CommandQueue, (void**)&q12)))
         return 2;
      iface12.interface_type    = RETRO_HW_RENDER_INTERFACE_D3D12;
      iface12.interface_version = RETRO_HW_RENDER_INTERFACE_D3D12_VERSION;
      iface12.device            = d12;
      iface12.queue             = q12;
      iface12.D3DCompile        = (pD3DCompile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
      iface12.required_state    = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      iface12.set_texture       = set_texture12;
   }

   core = LoadLibraryA(argv[1]);
   if (!core)
   {
      fprintf(stderr, "d3d_hdr: cannot load %s\n", argv[1]);
      return 2;
   }
#define CORE_FN(type, name) name = (type)GetProcAddress(core, #name)
   CORE_FN(set_env_t, retro_set_environment);
   CORE_FN(set_video_t, retro_set_video_refresh);
   CORE_FN(set_audio_t, retro_set_audio_sample);
   CORE_FN(set_audio_batch_t, retro_set_audio_sample_batch);
   CORE_FN(set_poll_t, retro_set_input_poll);
   CORE_FN(set_state_t, retro_set_input_state);
   CORE_FN(init_t, retro_init);
   CORE_FN(load_game_t, retro_load_game);
   CORE_FN(run_t, retro_run);
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
      fprintf(stderr, "d3d_hdr: retro_load_game failed\n");
      return finish(2);
   }
   if (!have_hw)
   {
      fprintf(stderr, "d3d_hdr: the core did not ask for Direct3D %d\n", api);
      return finish(1);
   }
   if (getenv("D3DH_REFUSED"))
   {
      iface11.device = NULL;
      iface12.queue  = NULL;
   }
   hw.context_reset();
   for (n = 0; n < frames; n++)
      retro_run();

   if (getenv("D3DH_REFUSED"))
   {
      printf("d3d_hdr: Direct3D %d refused, %u frames run, %s\n", api, frames,
            (api == 11 ? frame11 != NULL : frame12 != NULL) ? "a picture handed over" : "none drawn");
      return finish(0);
   }
   ok = api == 11 ? read_back11(argv[3], &format) : read_back12(argv[3], &format);
   if (!ok || frame_w == 0)
   {
      fprintf(stderr, "d3d_hdr: no picture was handed over\n");
      return finish(1);
   }
   expect = pix_fmt == RETRO_PIXEL_FORMAT_HDR10_2101010 ? DXGI_FORMAT_R10G10B10A2_UNORM
          : pix_fmt == RETRO_PIXEL_FORMAT_SCRGB_RGBA16F ? DXGI_FORMAT_R16G16B16A16_FLOAT
          : format;
   if (format != expect || (pix_fmt != RETRO_PIXEL_FORMAT_HDR10_2101010
         && pix_fmt != RETRO_PIXEL_FORMAT_SCRGB_RGBA16F && vkh_format(format) != VKH_RGBA8))
   {
      fprintf(stderr, "d3d_hdr: the pixel format is %d, the picture handed over DXGI format %d\n",
            (int)pix_fmt, (int)format);
      return finish(1);
   }
   if (up_render_w)
   {
      if (frame_w != up_render_w || frame_h != up_render_h)
      {
         fprintf(stderr, "d3d_hdr: the upscaler asked for %ux%u, the core drew %ux%u\n",
               up_render_w, up_render_h, frame_w, frame_h);
         return finish(1);
      }
      /* (the disc renders three frames in four through the tile accelerator) */
      if (up_count < frames / 2 || up_bad || !up_first_reset)
      {
         fprintf(stderr, "d3d_hdr: upscaler inputs for %u of %u frames, %u wrong, first reset %d\n",
               up_count, frames, up_bad, up_first_reset);
         return finish(1);
      }
      if (!check_upscaler_inputs12())
         return finish(1);
   }
   printf("d3d_hdr: Direct3D %d, %u frames, pixel format %d, picture %ux%u DXGI format %d%s\n",
         api, frames, (int)pix_fmt, frame_w, frame_h, (int)format, up_render_w ? ", upscaler inputs checked" : "");
   return finish(0);
}
