/* The HDR output's encode with Direct3D 11 and 12, through the encoders
 * the core uses (rend/hdr/hdr_encode_d3d11.c, hdr_encode_d3d12.c, and the
 * DXBC kernels_dxbc.sh made of hdr_encode.hlsl), over hdr_cases.h's
 * picture and cases: what each draws is read back and written out, as
 * floats, for tools/vulkan/hdr_hlsl --against to hold to the GLSL the
 * Vulkan renderer draws with. hdr_d3d.sh builds it with mingw-w64 and runs
 * it under wine.
 *
 *   hdr_d3d 11|12 out.bin
 *
 * C89. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdr_cases.h"
#include "../../core/rend/hdr/hdr_encode_d3d11.h"
#include "../../core/rend/hdr/hdr_encode_d3d12.h"

#define W HDR_CASES_W
#define H HDR_CASES_H

static FILE *out_file;
static unsigned cases, failures;
static float pixels[W * H * 4];
static unsigned char picture_px[W * H * 4];

static double half_to_float(uint16_t h)
{
   int e = (h >> 10) & 31, m = h & 1023;
   double v = e == 0 ? ldexp((double)m, -24) : e == 31 ? 65504.0 : ldexp((double)(m | 1024), e - 25);
   return (h & 0x8000u) ? -v : v;
}

/* @data rows @pitch apart, of @format, into pixels[] */
static void to_floats(const unsigned char *data, size_t pitch, DXGI_FORMAT format)
{
   unsigned x, y, c;
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float *p = pixels + ((size_t)y * W + x) * 4;
         if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
         {
            uint32_t v;
            memcpy(&v, data + y * pitch + x * 4, 4);
            for (c = 0; c < 3; c++)
               p[c] = (float)((v >> (10 * c)) & 1023u) / 1023.0f;
            p[3] = (float)(v >> 30) / 3.0f;
         }
         else
         {
            uint16_t v[4];
            memcpy(v, data + y * pitch + x * 8, 8);
            for (c = 0; c < 4; c++)
               p[c] = (float)half_to_float(v[c]);
         }
      }
}

static void put(void)
{
   if (fwrite(pixels, sizeof(pixels), 1, out_file) != 1)
   {
      fprintf(stderr, "hdr_d3d: cannot write\n");
      exit(1);
   }
   cases++;
}

/* (a cube case's tables made for its settings, then its cube baked) */
static void prepare(const struct hdr_dt_settings *dt, const struct hdr_output *o, int cube)
{
   struct hdr_dt_block b;
   hdr_dt_configure(dt);
   if (cube)
   {
      hdr_dt_fill(&b, o->paper_white_nits, o->max_nits, o->panel_peak_nits, o->panel_xy);
      hdr_dt_aces2_cube_finish(o->paper_white_nits);
      if (!hdr_dt_aces2_cube())
      {
         fprintf(stderr, "hdr_d3d: no cube\n");
         failures++;
      }
   }
}

/* ---- Direct3D 11 ---- */

static ID3D11Device *d11;
static ID3D11DeviceContext *c11;
static ID3D11ShaderResourceView *pic11;
static hdr_d3d11_t *enc11[2];

static void setup11(void)
{
   D3D11_TEXTURE2D_DESC td;
   D3D11_SUBRESOURCE_DATA sd;
   ID3D11Texture2D *tex;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &d11, NULL, &c11)))
   {
      fprintf(stderr, "hdr_d3d: no Direct3D 11 device\n");
      exit(2);
   }
   memset(&td, 0, sizeof(td));
   td.Width = W;
   td.Height = H;
   td.MipLevels = 1;
   td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_IMMUTABLE;
   td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   sd.pSysMem = picture_px;
   sd.SysMemPitch = W * 4;
   sd.SysMemSlicePitch = 0;
   if (FAILED(ID3D11Device_CreateTexture2D(d11, &td, &sd, &tex))
         || FAILED(ID3D11Device_CreateShaderResourceView(d11, (ID3D11Resource*)tex, NULL, &pic11)))
      exit(2);
   ID3D11Texture2D_Release(tex);
   enc11[0] = hdr_d3d11_new(d11, DXGI_FORMAT_R10G10B10A2_UNORM);
   enc11[1] = hdr_d3d11_new(d11, DXGI_FORMAT_R16G16B16A16_FLOAT);
   if (!enc11[0] || !enc11[1])
   {
      fprintf(stderr, "hdr_d3d: no Direct3D 11 encoder\n");
      exit(1);
   }
}

static void run11(const char *name, const struct hdr_dt_settings *dt, const struct hdr_output *o,
      int deband, int cube)
{
   const int scrgb = o->format == HDR_OUTPUT_SCRGB;
   const DXGI_FORMAT format = scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
   ID3D11Texture2D *texture = NULL, *staging;
   ID3D11ShaderResourceView *view = NULL;
   D3D11_TEXTURE2D_DESC td;
   D3D11_MAPPED_SUBRESOURCE map;
   prepare(dt, o, cube);
   /* (the frontend's frame index, as it goes round) */
   if (!hdr_d3d11_encode(enc11[scrgb], c11, o, pic11, W, H, deband != 0, cases % 3, &texture, &view))
   {
      fprintf(stderr, "hdr_d3d: %s: not encoded\n", name);
      exit(1);
   }
   ID3D11Texture2D_GetDesc(texture, &td);
   td.Usage = D3D11_USAGE_STAGING;
   td.BindFlags = 0;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   td.MiscFlags = 0;
   if (td.Format != format || FAILED(ID3D11Device_CreateTexture2D(d11, &td, NULL, &staging)))
      exit(1);
   ID3D11DeviceContext_CopyResource(c11, (ID3D11Resource*)staging, (ID3D11Resource*)texture);
   if (FAILED(ID3D11DeviceContext_Map(c11, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &map)))
      exit(1);
   to_floats((const unsigned char*)map.pData, map.RowPitch, format);
   ID3D11DeviceContext_Unmap(c11, (ID3D11Resource*)staging, 0);
   ID3D11Texture2D_Release(staging);
   put();
}

/* ---- Direct3D 12 ---- */

static ID3D12Device *d12;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_value;
static ID3D12Resource *pic12, *readback;
static hdr_d3d12_t *enc12[2];

static ID3D12Resource *buffer12(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
   D3D12_HEAP_PROPERTIES hp;
   D3D12_RESOURCE_DESC rd;
   ID3D12Resource *r = NULL;
   memset(&hp, 0, sizeof(hp));
   hp.Type = type;
   memset(&rd, 0, sizeof(rd));
   rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   rd.Width = size;
   rd.Height = 1;
   rd.DepthOrArraySize = 1;
   rd.MipLevels = 1;
   rd.SampleDesc.Count = 1;
   rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   if (FAILED(ID3D12Device_CreateCommittedResource(d12, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
         &IID_ID3D12Resource, (void**)&r)))
      exit(2);
   return r;
}

static void barrier12(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b;
   memset(&b, 0, sizeof(b));
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition.pResource = r;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = from;
   b.Transition.StateAfter = to;
   ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* @list run and waited for (a test: the core never waits) */
static void submit12(void)
{
   ID3D12CommandList *l = (ID3D12CommandList*)list;
   ID3D12GraphicsCommandList_Close(list);
   ID3D12CommandQueue_ExecuteCommandLists(queue, 1, &l);
   ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
   if (ID3D12Fence_GetCompletedValue(fence) < fence_value)
   {
      ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
      WaitForSingleObject(fence_event, INFINITE);
   }
   ID3D12CommandAllocator_Reset(alloc);
   ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
}

static void copy12(ID3D12Resource *dst, ID3D12Resource *src, DXGI_FORMAT format, unsigned bpp, int to_texture)
{
   D3D12_TEXTURE_COPY_LOCATION t, b;
   memset(&t, 0, sizeof(t));
   memset(&b, 0, sizeof(b));
   t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   t.SubresourceIndex = 0;
   b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   b.PlacedFootprint.Footprint.Format = format;
   b.PlacedFootprint.Footprint.Width = W;
   b.PlacedFootprint.Footprint.Height = H;
   b.PlacedFootprint.Footprint.Depth = 1;
   b.PlacedFootprint.Footprint.RowPitch = W * bpp;
   if (to_texture)
   {
      t.pResource = dst;
      b.pResource = src;
      ID3D12GraphicsCommandList_CopyTextureRegion(list, &t, 0, 0, 0, &b, NULL);
   }
   else
   {
      t.pResource = src;
      b.pResource = dst;
      ID3D12GraphicsCommandList_CopyTextureRegion(list, &b, 0, 0, 0, &t, NULL);
   }
}

static void setup12(void)
{
   D3D12_COMMAND_QUEUE_DESC qd;
   D3D12_HEAP_PROPERTIES hp;
   D3D12_RESOURCE_DESC rd;
   ID3D12Resource *upload;
   void *map;
   if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&d12)))
   {
      fprintf(stderr, "hdr_d3d: no Direct3D 12 device\n");
      exit(2);
   }
   memset(&qd, 0, sizeof(qd));
   qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
   if (FAILED(ID3D12Device_CreateCommandQueue(d12, &qd, &IID_ID3D12CommandQueue, (void**)&queue))
         || FAILED(ID3D12Device_CreateCommandAllocator(d12, D3D12_COMMAND_LIST_TYPE_DIRECT,
               &IID_ID3D12CommandAllocator, (void**)&alloc))
         || FAILED(ID3D12Device_CreateCommandList(d12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
               &IID_ID3D12GraphicsCommandList, (void**)&list))
         || FAILED(ID3D12Device_CreateFence(d12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence)))
      exit(2);
   fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

   memset(&hp, 0, sizeof(hp));
   hp.Type = D3D12_HEAP_TYPE_DEFAULT;
   memset(&rd, 0, sizeof(rd));
   rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   rd.Width = W;
   rd.Height = H;
   rd.DepthOrArraySize = 1;
   rd.MipLevels = 1;
   rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   rd.SampleDesc.Count = 1;
   if (FAILED(ID3D12Device_CreateCommittedResource(d12, &hp, D3D12_HEAP_FLAG_NONE, &rd,
         D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&pic12)))
      exit(2);
   upload = buffer12(D3D12_HEAP_TYPE_UPLOAD, (UINT64)W * H * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
   ID3D12Resource_Map(upload, 0, NULL, &map);
   memcpy(map, picture_px, sizeof(picture_px));
   ID3D12Resource_Unmap(upload, 0, NULL);
   copy12(pic12, upload, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);
   barrier12(pic12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   submit12();
   ID3D12Resource_Release(upload);
   readback = buffer12(D3D12_HEAP_TYPE_READBACK, (UINT64)W * H * 8, D3D12_RESOURCE_STATE_COPY_DEST);

   enc12[0] = hdr_d3d12_new(d12, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, fence);
   enc12[1] = hdr_d3d12_new(d12, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, fence);
   if (!enc12[0] || !enc12[1])
   {
      fprintf(stderr, "hdr_d3d: no Direct3D 12 encoder\n");
      exit(1);
   }
}

static void run12(const char *name, const struct hdr_dt_settings *dt, const struct hdr_output *o,
      int deband, int cube)
{
   const int scrgb = o->format == HDR_OUTPUT_SCRGB;
   const DXGI_FORMAT format = scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
   const unsigned bpp = scrgb ? 8 : 4;
   ID3D12Resource *texture = NULL;
   void *map;
   prepare(dt, o, cube);
   if (!hdr_d3d12_encode(enc12[scrgb], list, o, pic12, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, deband != 0,
         cases % 3, fence_value + 1, &texture))
   {
      fprintf(stderr, "hdr_d3d: %s: not encoded\n", name);
      exit(1);
   }
   barrier12(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
   copy12(readback, texture, format, bpp, 0);
   barrier12(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   submit12();
   ID3D12Resource_Map(readback, 0, NULL, &map);
   to_floats((const unsigned char*)map, (size_t)W * bpp, format);
   ID3D12Resource_Unmap(readback, 0, NULL);
   put();
}

int main(int argc, char **argv)
{
   int api;
   if (argc != 3 || (strcmp(argv[1], "11") && strcmp(argv[1], "12")))
   {
      fprintf(stderr, "usage: hdr_d3d 11|12 out.bin\n");
      return 2;
   }
   api = atoi(argv[1]);
   out_file = fopen(argv[2], "wb");
   if (!out_file)
      return 2;
   hdr_cases_picture(picture_px);
   if (api == 11)
   {
      setup11();
      hdr_cases_each(run11);
      hdr_d3d11_free(enc11[0]);
      hdr_d3d11_free(enc11[1]);
   }
   else
   {
      setup12();
      hdr_cases_each(run12);
      hdr_d3d12_free(enc12[0]);
      hdr_d3d12_free(enc12[1]);
   }
   hdr_dt_free();
   fclose(out_file);
   printf("hdr_d3d: Direct3D %d, %u cases drawn%s\n", api, cases, failures ? ", with failures" : "");
   return failures ? 1 : 0;
}
