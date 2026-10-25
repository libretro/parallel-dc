/* The HDR output's encode with Direct3D 11 and 12, through the encoders
 * the core uses (rend/hdr/hdr_encode_d3d11.c, hdr_encode_d3d12.c, and the
 * DXBC kernels_dxbc.sh made of hdr_encode.hlsl), over hdr_cases.h's
 * picture and cases: what each draws is read back and written out, as
 * floats, for tools/vulkan/hdr_hlsl --against to hold to the GLSL the
 * Vulkan renderer draws with. hdr_d3d.sh builds it with mingw-w64 and runs
 * it under wine.
 *
 *   hdr_d3d 11|12 out.bin
 *   hdr_d3d 12ssaa
 *   hdr_d3d post11|post12
 *
 * 12ssaa: the Direct3D 12 encoder with SSAA's samples (hdr_encode.hlsl's
 * HDR_ENCODE_SSAA). Known values are drawn into each sample of a 4x
 * multisampled picture, the picture is made their resolve by the shared
 * rule (rend/tile/pvr_resolve.h), but for a strip drawn over (the VMUs'
 * stand-in); the encode of the two is held to that of the samples' exact
 * average as a half-float picture - the picture where it was drawn over -
 * and has to differ from that of the 8-bit picture: the extra bits are
 * there and are the samples'.
 *
 * post11, post12: the encoder with the HDR post effects (bloom in each
 * of its shapes, halation, chromatic aberration, Compact Intermediates),
 * scRGB, held to what tools/hdr/hdr_post_ref.c makes of the picture on
 * the CPU.
 *
 * C89. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdr_cases.h"
#include "../../core/rend/hdr/hdr_encode_d3d11.h"
#include "../../core/rend/hdr/hdr_encode_d3d12.h"
#include "hdr_post_ref.h"

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

/* (ACES 2.0 (full)'s tables made now, which frames leave to a worker,
 * and a cube case's cube from them) */
static void prepare(const struct hdr_dt_settings *dt, const struct hdr_output *o, int cube)
{
   hdr_dt_configure(dt);
   hdr_dt_finish(o->paper_white_nits, o->max_nits, o->panel_peak_nits, o->panel_xy, cube != 0);
   if (cube)
   {
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
         NULL, 0, cases % 3, fence_value + 1, &texture))
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

/* ---- Direct3D 12 with SSAA's samples ---- */

#define SSAA_N    4
#define SSAA_LOG2 2
#define OSD_X     16   /* the columns left of it are "drawn over" */

/* The byte the filler draws at channel @c of sample @s of (x, y): its PS's arithmetic */
static unsigned sample_byte(unsigned x, unsigned y, unsigned s, unsigned c)
{
   unsigned v = (x * 73u + y * 151u + s * 61u + c * 97u) * 2654435761u;
   /* (a smooth ramp across x, so most samples differ by a few steps, with the hash's low bits) */
   return ((x + (v >> 30)) * 255u / (W + 3u)) & 255u;
}

static uint16_t to_half(float f)
{
   union { float f; uint32_t u; } v;
   uint32_t sign, mant;
   int exp;
   v.f  = f;
   sign = (v.u >> 16) & 0x8000u;
   exp  = (int)((v.u >> 23) & 0xff) - 127 + 15;
   mant = v.u & 0x7fffffu;
   if (exp <= 0)
      return (uint16_t)sign;
   if (exp >= 31)
      return (uint16_t)(sign | 0x7bffu);
   {
      uint32_t h = ((uint32_t)exp << 10) | (mant >> 13);
      if ((mant & 0x1fffu) > 0x1000u || ((mant & 0x1fffu) == 0x1000u && (h & 1u)))
         h++;
      return (uint16_t)(sign | h);
   }
}

static ID3D12Resource *texture12(DXGI_FORMAT format, unsigned samples, D3D12_RESOURCE_FLAGS flags,
      D3D12_RESOURCE_STATES state)
{
   D3D12_HEAP_PROPERTIES hp;
   D3D12_RESOURCE_DESC rd;
   ID3D12Resource *r = NULL;
   memset(&hp, 0, sizeof(hp));
   hp.Type = D3D12_HEAP_TYPE_DEFAULT;
   memset(&rd, 0, sizeof(rd));
   rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   rd.Width = W;
   rd.Height = H;
   rd.DepthOrArraySize = 1;
   rd.MipLevels = 1;
   rd.Format = format;
   rd.SampleDesc.Count = samples;
   rd.Flags = flags;
   if (FAILED(ID3D12Device_CreateCommittedResource(d12, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
         &IID_ID3D12Resource, (void**)&r)))
      exit(2);
   return r;
}

/* @data (W x H of @bpp bytes) into a new texture of @format, in the pixel shader resource state */
static ID3D12Resource *uploaded12(const void *data, DXGI_FORMAT format, unsigned bpp)
{
   ID3D12Resource *t = texture12(format, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *up = buffer12(D3D12_HEAP_TYPE_UPLOAD, (UINT64)W * H * bpp, D3D12_RESOURCE_STATE_GENERIC_READ);
   void *map;
   ID3D12Resource_Map(up, 0, NULL, &map);
   memcpy(map, data, (size_t)W * H * bpp);
   ID3D12Resource_Unmap(up, 0, NULL);
   copy12(t, up, format, bpp, 1);
   barrier12(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   submit12();
   ID3D12Resource_Release(up);
   return t;
}

static ID3DBlob *compile(const char *src, const char *target)
{
   pD3DCompile fn = (pD3DCompile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
   ID3DBlob *code = NULL, *err = NULL;
   if (!fn || FAILED(fn(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &code, &err)))
   {
      fprintf(stderr, "hdr_d3d: the filler did not compile: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
      exit(2);
   }
   return code;
}

/* Each sample of @ms drawn with sample_byte()'s values: a draw for each,
 * the pipeline's sample mask picking it and a root constant saying which
 * (SV_SampleIndex is not in every compiler wine has) */
static void fill_samples(ID3D12Resource *ms)
{
   static const char vs[] =
      "float4 main(uint id : SV_VertexID) : SV_Position {"
      " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }";
   static const char ps[] =
      "uint b(uint x, uint y, uint s, uint c) { uint v = (x * 73u + y * 151u + s * 61u + c * 97u) * 2654435761u;"
      " return ((x + (v >> 30)) * 255u / (256u + 3u)) & 255u; }"
      "cbuffer S : register(b0) { uint s; };"
      "float4 main(float4 pos : SV_Position) : SV_Target {"
      " uint x = (uint)pos.x, y = (uint)pos.y;"
      " return float4(b(x, y, s, 0u), b(x, y, s, 1u), b(x, y, s, 2u), b(x, y, s, 3u)) / 255.0; }";
   D3D12_ROOT_SIGNATURE_DESC rsd;
   D3D12_ROOT_PARAMETER rp;
   ID3DBlob *rsb = NULL, *rse = NULL, *vsb = compile(vs, "vs_5_0"), *psb = compile(ps, "ps_5_0");
   ID3D12RootSignature *root = NULL;
   ID3D12PipelineState *pso[SSAA_N];
   unsigned n;
   D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
   D3D12_DESCRIPTOR_HEAP_DESC hd;
   ID3D12DescriptorHeap *rtv_heap;
   D3D12_CPU_DESCRIPTOR_HANDLE rtv;
   D3D12_VIEWPORT vp;
   D3D12_RECT sc;
   memset(&rp, 0, sizeof(rp));
   rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   rp.Constants.Num32BitValues = 1;
   rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
   memset(&rsd, 0, sizeof(rsd));
   rsd.NumParameters = 1;
   rsd.pParameters = &rp;
   if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse))
         || FAILED(ID3D12Device_CreateRootSignature(d12, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
               &IID_ID3D12RootSignature, (void**)&root)))
      exit(2);
   memset(&pd, 0, sizeof(pd));
   pd.pRootSignature = root;
   pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb);
   pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
   pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb);
   pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
   pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
   pd.SampleMask = 0xffffffffu;
   pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
   pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
   pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   pd.NumRenderTargets = 1;
   pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
   pd.SampleDesc.Count = SSAA_N;
   for (n = 0; n < SSAA_N; n++)
   {
      pd.SampleMask = 1u << n;
      if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d12, &pd, &IID_ID3D12PipelineState, (void**)&pso[n])))
         exit(2);
   }
   memset(&hd, 0, sizeof(hd));
   hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
   hd.NumDescriptors = 1;
   if (FAILED(ID3D12Device_CreateDescriptorHeap(d12, &hd, &IID_ID3D12DescriptorHeap, (void**)&rtv_heap)))
      exit(2);
   rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
   ID3D12Device_CreateRenderTargetView(d12, ms, NULL, rtv);
   vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f; vp.Width = (float)W; vp.Height = (float)H; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
   sc.left = 0; sc.top = 0; sc.right = W; sc.bottom = H;
   ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
   ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
   ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
   ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
   ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   for (n = 0; n < SSAA_N; n++)
   {
      ID3D12GraphicsCommandList_SetPipelineState(list, pso[n]);
      ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(list, 0, n, 0);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
   }
   barrier12(ms, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   submit12();
}

/* What @picture (of @format, 8 bits a channel when @unorm8; SSAA's @samples or NULL) encodes to, as floats */
static void encode12(hdr_d3d12_t *e, const struct hdr_output *o, ID3D12Resource *picture, DXGI_FORMAT format,
      bool unorm8, ID3D12Resource *samples, float *dst)
{
   const int scrgb = o->format == HDR_OUTPUT_SCRGB;
   const DXGI_FORMAT out = scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
   const unsigned bpp = scrgb ? 8 : 4;
   ID3D12Resource *texture = NULL;
   void *map;
   if (!hdr_d3d12_encode(e, list, o, picture, format, W, H, unorm8, samples, samples ? SSAA_LOG2 : 0,
         cases % 3, fence_value + 1, &texture))
   {
      fprintf(stderr, "hdr_d3d: not encoded\n");
      exit(1);
   }
   barrier12(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
   copy12(readback, texture, out, bpp, 0);
   barrier12(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   submit12();
   ID3D12Resource_Map(readback, 0, NULL, &map);
   to_floats((const unsigned char*)map, (size_t)W * bpp, out);
   ID3D12Resource_Unmap(readback, 0, NULL);
   memcpy(dst, pixels, sizeof(pixels));
   cases++;
}

static float ssaa_out[W * H * 4], exact_out[W * H * 4], byte_out[W * H * 4];

static int ssaa12(void)
{
   static unsigned char pic8[W * H * 4];
   static uint16_t exact16[W * H * 4];
   struct hdr_dt_settings dt;
   struct hdr_output o;
   ID3D12Resource *ms, *pic, *exact;
   unsigned x, y, c, i, k;

   setup12();
   ms = texture12(DXGI_FORMAT_R8G8B8A8_UNORM, SSAA_N, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
         D3D12_RESOURCE_STATE_RENDER_TARGET);
   fill_samples(ms);
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
         for (c = 0; c < 4; c++)
         {
            const size_t at = ((size_t)y * W + x) * 4 + c;
            unsigned sum = 0, s;
            for (s = 0; s < SSAA_N; s++)
               sum += sample_byte(x, y, s, c);
            if (x < OSD_X)
            {
               /* drawn over: not the resolve (a byte it cannot be: off by 7) */
               pic8[at] = (unsigned char)(((sum + SSAA_N / 2) / SSAA_N + 7u) & 255u);
               exact16[at] = to_half((float)pic8[at] / 255.0f);
            }
            else
            {
               pic8[at] = (unsigned char)((sum + SSAA_N / 2) / SSAA_N);   /* pvr_resolve_join() */
               exact16[at] = to_half((float)sum / (255.0f * SSAA_N));
            }
         }
   pic = uploaded12(pic8, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
   exact = uploaded12(exact16, DXGI_FORMAT_R16G16B16A16_FLOAT, 8);

   memset(&dt, 0, sizeof(dt));
   dt.aces2_surround = 1;
   dt.gt_toe = 0.22f;
   dt.gt_linear = 0.40f;
   dt.filmic_contrast = 3;
   dt.filmic_max_ev = 4.026068811f;
   memset(&o, 0, sizeof(o));
   o.paper_white_nits = 200.0f;
   o.max_nits = 1000.0f;
   for (k = 0; k < 4; k++)
   {
      unsigned differ = 0, osd_differ = 0;
      double worst = 0.0;
      dt.curve = (k & 2) ? HDR_DT_AGX : HDR_DT_KNEE;
      o.format = (k & 1) ? HDR_OUTPUT_SCRGB : HDR_OUTPUT_HDR10;
      hdr_dt_configure(&dt);
      hdr_dt_finish(o.paper_white_nits, o.max_nits, o.panel_peak_nits, o.panel_xy, false);
      encode12(enc12[k & 1], &o, pic, DXGI_FORMAT_R8G8B8A8_UNORM, true, ms, ssaa_out);
      encode12(enc12[k & 1], &o, exact, DXGI_FORMAT_R16G16B16A16_FLOAT, false, NULL, exact_out);
      encode12(enc12[k & 1], &o, pic, DXGI_FORMAT_R8G8B8A8_UNORM, false, NULL, byte_out);
      for (i = 0; i < W * H * 4; i++)
      {
         const double a = ssaa_out[i], b = exact_out[i];
         /* the half-float picture's rounding and the output's: a 10-bit code (the two
          * on either side of a rounding, both quantized), or a half float's */
         const double tol = (o.format == HDR_OUTPUT_SCRGB) ? 2e-3 * (fabs(b) > 1.0 ? fabs(b) : 1.0) : 1.001 / 1023.0;
         const double d = fabs(a - b) / tol;
         if (d > worst)
            worst = d;
         if (i % 4 != 3 && fabs(ssaa_out[i] - byte_out[i]) > 1e-6)
         {
            if ((i / 4) % W < OSD_X)
               osd_differ++;
            else
               differ++;
         }
      }
      printf("hdr_d3d: SSAA %s %s: within %.2f of the tolerance of the exact average's; %u channels not the 8-bit picture's, %u drawn over\n",
            (k & 2) ? "agx" : "knee", (k & 1) ? "scRGB" : "HDR10", worst, differ, osd_differ);
      if (!(worst <= 1.0) || differ < 1000 || osd_differ != 0)
         failures++;
   }
   hdr_d3d12_free(enc12[0]);
   hdr_d3d12_free(enc12[1]);
   hdr_dt_free();
   printf("hdr_d3d: Direct3D 12 with SSAA's samples%s\n", failures ? ": failed" : ": the samples' exact average");
   return failures ? 1 : 0;
}

/* ---- the post effects ---- */

static uint16_t post_out[W * H * 4];

static int post_run(int api)
{
   static const char *const sets[][4] = {
      { "reicast_hdr_bloom=standard", NULL },
      { "reicast_hdr_bloom=intense", "reicast_hdr_bloom_range=widest", "reicast_hdr_halation=strong", NULL },
      { "reicast_hdr_bloom=subtle", "reicast_hdr_bloom_convolution=enabled", "reicast_hdr_bloom_select=knee", NULL },
      { "reicast_hdr_chromatic=heavy", NULL },
      { "reicast_hdr_bloom=standard", "reicast_hdr_chromatic=subtle", "reicast_hdr_post_compact=enabled", NULL }
   };
   struct hdr_output o;
   unsigned          i;
   int               bad = 0;

   memset(&o, 0, sizeof(o));
   o.format           = HDR_OUTPUT_SCRGB;
   o.paper_white_nits = 200.0f;
   o.max_nits         = 1000.0f;
   hdr_cases_picture(picture_px);
   if (api == 11)
      setup11();
   else
      setup12();
   for (i = 0; i < sizeof(sets) / sizeof(sets[0]); i++)
   {
      const char *keys[4], *vals[4];
      char        buf[4][64];
      int         n = 0, k;
      for (k = 0; sets[i][k]; k++)
      {
         const char *eq = strchr(sets[i][k], '=');
         memcpy(buf[k], sets[i][k], (size_t)(eq - sets[i][k]));
         buf[k][eq - sets[i][k]] = '\0';
         keys[n]   = buf[k];
         vals[n++] = eq + 1;
      }
      hdr_post_ref_configure(keys, vals, n);
      /* (the options' transform, what the frontend says of the display) */
      o.sdr_eotf = 0;
      o.shoulder = 0;
      if (api == 11)
      {
         ID3D11Texture2D *texture = NULL, *staging;
         ID3D11ShaderResourceView *view = NULL;
         D3D11_TEXTURE2D_DESC td;
         D3D11_MAPPED_SUBRESOURCE map;
         if (!hdr_d3d11_encode(enc11[1], c11, &o, pic11, W, H, true, i % 3, &texture, &view))
            return 1;
         ID3D11Texture2D_GetDesc(texture, &td);
         td.Usage = D3D11_USAGE_STAGING;
         td.BindFlags = 0;
         td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         td.MiscFlags = 0;
         if (FAILED(ID3D11Device_CreateTexture2D(d11, &td, NULL, &staging)))
            return 1;
         ID3D11DeviceContext_CopyResource(c11, (ID3D11Resource*)staging, (ID3D11Resource*)texture);
         if (FAILED(ID3D11DeviceContext_Map(c11, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &map)))
            return 1;
         for (k = 0; k < H; k++)
            memcpy(post_out + (size_t)k * W * 4, (const unsigned char*)map.pData + (size_t)k * map.RowPitch, (size_t)W * 8);
         ID3D11DeviceContext_Unmap(c11, (ID3D11Resource*)staging, 0);
         ID3D11Texture2D_Release(staging);
      }
      else
      {
         ID3D12Resource *texture = NULL;
         void *map;
         if (!hdr_d3d12_encode(enc12[1], list, &o, pic12, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, true,
               NULL, 0, i % 3, fence_value + 1, &texture))
            return 1;
         barrier12(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
         copy12(readback, texture, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 0);
         barrier12(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
         submit12();
         ID3D12Resource_Map(readback, 0, NULL, &map);
         memcpy(post_out, map, sizeof(post_out));
         ID3D12Resource_Unmap(readback, 0, NULL);
      }
      printf("hdr_d3d: Direct3D %d, post effects %u: ", api, i);
      fflush(stdout);
      if (hdr_post_ref_check(picture_px, W, H, post_out, half_to_float, keys, vals, n))
         bad++;
   }
   if (api == 11)
      hdr_d3d11_free(enc11[1]);
   else
      hdr_d3d12_free(enc12[1]);
   hdr_dt_free();
   return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
   int api;
   if (argc == 2 && !strcmp(argv[1], "12ssaa"))
      return ssaa12();
   if (argc == 2 && (!strcmp(argv[1], "post11") || !strcmp(argv[1], "post12")))
      return post_run(argv[1][4] == '1' && argv[1][5] == '1' ? 11 : 12);
   if (argc != 3 || (strcmp(argv[1], "11") && strcmp(argv[1], "12")))
   {
      fprintf(stderr, "usage: hdr_d3d 11|12 out.bin | 12ssaa\n");
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
