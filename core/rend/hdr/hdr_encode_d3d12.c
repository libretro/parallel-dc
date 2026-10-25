/* See hdr_encode_d3d12.h. C89.
 *
 * Recorded into the core's own command list, after the frame, and
 * submitted with it: the picture is in the same list or one before it on
 * the same queue. What the encode writes into for a frame - its constants,
 * its descriptors - is a slot of a ring longer than the core's ring of
 * command allocators, so a slot is never one the GPU still reads by the
 * time it comes round; what goes (an output of the size before, the
 * tables of the settings before, their uploads) goes with a slot when it
 * comes round again. */
#define COBJMACROS
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "hdr_encode_d3d12.h"
#include "hdr_output.h"
#include "hdr_transform.h"
#include "hdr_encode_dxbc.inc"

/* (more than the core's DX12Context::ALLOCATORS, which is 8) */
#define HDR_D3D12_SLOTS       10
#define HDR_D3D12_MAX_OUTPUTS 32
/* What a frame can retire: the four tables, their uploads, one output and
 * the post effects' images */
#define HDR_D3D12_MAX_RETIRED (10 + 1 + HDR_POST_IMAGES)
/* the transform's constants, in a slot of the upload ring (a constant
 * buffer's place is a multiple of 256 bytes) */
#define HDR_D3D12_DT_STRIDE   512
/* a slot's descriptors: the transform's constants (b0), then the picture
 * and the tables (t0-t4) - b0 in the table rather than at the root, since
 * vkd3d cannot have a root CBV and static samplers both on drivers with
 * eight descriptor sets (lavapipe's) */
#define HDR_D3D12_SRVS        7    /* b0; t0 the picture, t1-t4 the tables, t5 SSAA's samples */
/* The post effects' slot of their heap: the encode's (its first pass),
 * the composite's (b0; t0 the scene, t1-t4 the tables, t5-t6 the bands)
 * and each bloom pass's source */
#define HDR_D3D12_POST_COMPOSITE HDR_D3D12_SRVS
#define HDR_D3D12_POST_BLOOM     (HDR_D3D12_POST_COMPOSITE + 8)
#define HDR_D3D12_POST_SRVS      (HDR_D3D12_POST_BLOOM + HDR_POST_PASSES)

static const IID iid_root_signature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID iid_pipeline_state = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID iid_descriptor_heap = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };
static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

typedef HRESULT (WINAPI *serialize_root_t)(const D3D12_ROOT_SIGNATURE_DESC *,
      D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

enum
{
   T_ACES2_LUT = 0,
   T_FILM_LUT,
   T_DESAT,
   T_CUBE,
   T_DUMMY_2D,
   T_DUMMY_3D,
   T_COUNT
};

typedef struct hdr_d3d12_slot
{
   uint64_t        signal;    /* what the fence says when the GPU is done with it */
   ID3D12Resource *retired[HDR_D3D12_MAX_RETIRED];
   unsigned        retired_count;
} hdr_d3d12_slot_t;

typedef struct hdr_d3d12_output
{
   ID3D12Resource *texture;
   unsigned        width;
   unsigned        height;
} hdr_d3d12_output_t;

/* The post effects' intermediates: RGBA16F, or (Compact Intermediates)
 * R11G11B10 */
enum
{
   P_HALF = 0,
   P_COMPACT,
   P_FORMATS
};

typedef struct hdr_d3d12_image
{
   ID3D12Resource *texture;   /* in the pixel shader resource state between passes */
   unsigned        width;
   unsigned        height;
   int             format;
} hdr_d3d12_image_t;

/* What draws them, made the first time they are on and kept */
typedef struct hdr_d3d12_post
{
   int                   made;
   ID3D12RootSignature  *root_bloom;
   ID3D12RootSignature  *root_composite;
   ID3D12PipelineState  *composite;
   ID3D12PipelineState  *linear[P_FORMATS];
   ID3D12PipelineState  *linear_ssaa[P_FORMATS];
   ID3D12PipelineState  *linear_sums[P_FORMATS];
   ID3D12PipelineState  *stage[P_FORMATS][HDR_BLOOM_STAGES];
   int                   format_made[P_FORMATS];
   ID3D12DescriptorHeap *srv_heap;   /* HDR_D3D12_POST_SRVS a slot */
   ID3D12DescriptorHeap *rtv_heap;   /* the scene, then the images */
   hdr_d3d12_image_t     scene;
   hdr_d3d12_image_t     images[HDR_POST_IMAGES];
} hdr_d3d12_post_t;

struct hdr_d3d12
{
   ID3D12Device          *device;
   ID3D12Fence           *fence;
   DXGI_FORMAT            format;
   D3D12_RESOURCE_STATES  handed;
   ID3D12RootSignature   *root;
   ID3D12PipelineState   *pso;
   ID3D12PipelineState   *pso_ssaa;   /* with SSAA's samples (hdr_encode.hlsl's HDR_ENCODE_SSAA) */
   ID3D12PipelineState   *pso_sums;   /* ...or their channel sums (HDR_ENCODE_SUMS) */
   ID3D12DescriptorHeap  *srv_heap;   /* shader visible: HDR_D3D12_SRVS a slot */
   ID3D12DescriptorHeap  *rtv_heap;   /* one an output */
   UINT                   srv_step;
   UINT                   rtv_step;
   ID3D12Resource        *upload;     /* the transform's constants, a slot each */
   unsigned char         *upload_map;
   hdr_d3d12_slot_t       slots[HDR_D3D12_SLOTS];
   unsigned               slot;
   hdr_d3d12_output_t     outputs[HDR_D3D12_MAX_OUTPUTS];
   ID3D12Resource        *tables[T_COUNT];
   unsigned               generation;
   hdr_d3d12_post_t       post;
};

#define RELEASE(type, p) do { if (p) { type##_Release(p); (p) = NULL; } } while (0)

/* (the struct returns, called as Microsoft's ABI has them: an out pointer) */
static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12DescriptorHeap *heap)
{
   D3D12_CPU_DESCRIPTOR_HANDLE out;
   ((void (STDMETHODCALLTYPE *)(ID3D12DescriptorHeap *, D3D12_CPU_DESCRIPTOR_HANDLE *))
      heap->lpVtbl->GetCPUDescriptorHandleForHeapStart)(heap, &out);
   return out;
}

static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(ID3D12DescriptorHeap *heap)
{
   D3D12_GPU_DESCRIPTOR_HANDLE out;
   ((void (STDMETHODCALLTYPE *)(ID3D12DescriptorHeap *, D3D12_GPU_DESCRIPTOR_HANDLE *))
      heap->lpVtbl->GetGPUDescriptorHandleForHeapStart)(heap, &out);
   return out;
}

static void barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
      D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b;
   if (from == to)
      return;
   memset(&b, 0, sizeof(b));
   b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition.pResource   = res;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = from;
   b.Transition.StateAfter  = to;
   ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);
}

/* Into the current slot's list, which a frame cannot fill: it is emptied
 * each time the slot comes round. */
static void retire(hdr_d3d12_t *e, ID3D12Resource *res)
{
   hdr_d3d12_slot_t *s = &e->slots[e->slot];
   if (res && s->retired_count < HDR_D3D12_MAX_RETIRED)
      s->retired[s->retired_count++] = res;
}

static void slot_release(hdr_d3d12_slot_t *s)
{
   unsigned i;
   for (i = 0; i < s->retired_count; i++)
      ID3D12Resource_Release(s->retired[i]);
   s->retired_count = 0;
}

static ID3D12Resource *committed(hdr_d3d12_t *e, D3D12_HEAP_TYPE heap,
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state)
{
   D3D12_HEAP_PROPERTIES props;
   ID3D12Resource       *res = NULL;
   memset(&props, 0, sizeof(props));
   props.Type = heap;
   if (FAILED(ID3D12Device_CreateCommittedResource(e->device, &props, D3D12_HEAP_FLAG_NONE,
               desc, state, NULL, &iid_resource, (void **)&res)))
      return NULL;
   return res;
}

static ID3D12Resource *buffer_upload(hdr_d3d12_t *e, UINT64 size)
{
   D3D12_RESOURCE_DESC desc;
   memset(&desc, 0, sizeof(desc));
   desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
   desc.Width            = size;
   desc.Height           = 1;
   desc.DepthOrArraySize = 1;
   desc.MipLevels        = 1;
   desc.SampleDesc.Count = 1;
   desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   return committed(e, D3D12_HEAP_TYPE_UPLOAD, &desc, D3D12_RESOURCE_STATE_GENERIC_READ);
}

/* A table as an RGBA16F texture, half floats being the 16-bit format
 * every device filters: @data unorm16 of @channels a texel (or half RGBA
 * when @half), NULL for a texel of zero; 3D or 2D. Its upload is recorded
 * into @cmd and retired with the slot; it is left shader-readable. */
static ID3D12Resource *table_make(hdr_d3d12_t *e, ID3D12GraphicsCommandList *cmd,
      const uint16_t *data, unsigned w, unsigned h, unsigned d,
      unsigned channels, bool half, bool three_d)
{
   D3D12_RESOURCE_DESC                desc;
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;
   D3D12_TEXTURE_COPY_LOCATION        dst, src;
   UINT                               rows;
   UINT64                             row_size, total;
   ID3D12Resource                    *tex, *up;
   unsigned char                     *map = NULL;
   D3D12_RANGE                        none;
   unsigned                           x, y, z, c;

   memset(&desc, 0, sizeof(desc));
   desc.Dimension        = three_d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   desc.Width            = w;
   desc.Height           = h;
   desc.DepthOrArraySize = (UINT16)d;
   desc.MipLevels        = 1;
   desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
   desc.SampleDesc.Count = 1;
   if (!(tex = committed(e, D3D12_HEAP_TYPE_DEFAULT, &desc, D3D12_RESOURCE_STATE_COPY_DEST)))
      return NULL;
   ID3D12Device_GetCopyableFootprints(e->device, &desc, 0, 1, 0, &foot, &rows, &row_size, &total);
   if (!(up = buffer_upload(e, total)))
   {
      ID3D12Resource_Release(tex);
      return NULL;
   }
   none.Begin = 0;
   none.End   = 0;
   if (FAILED(ID3D12Resource_Map(up, 0, &none, (void **)&map)))
   {
      ID3D12Resource_Release(up);
      ID3D12Resource_Release(tex);
      return NULL;
   }
   for (z = 0; z < d; z++)
      for (y = 0; y < h; y++)
      {
         uint16_t *row = (uint16_t*)(map + foot.Offset
               + ((UINT64)z * rows + y) * foot.Footprint.RowPitch);
         for (x = 0; x < w; x++)
         {
            size_t i = ((size_t)z * h + y) * w + x;
            for (c = 0; c < 4; c++)
               row[x * 4 + c] = !data ? 0 : half ? data[i * 4 + c]
                  : hdr_dt_unorm16_to_half(data[i * channels + (c < channels ? c : 0)]);
         }
      }
   ID3D12Resource_Unmap(up, 0, NULL);

   memset(&dst, 0, sizeof(dst));
   dst.pResource        = tex;
   dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   dst.SubresourceIndex = 0;
   memset(&src, 0, sizeof(src));
   src.pResource        = up;
   src.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   src.PlacedFootprint  = foot;
   ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &dst, 0, 0, 0, &src, NULL);
   barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   retire(e, up);
   return tex;
}

/* The tables again when hdr_transform.c has new ones: a table it does not
 * have is left out, and the shader reads the stand-in. */
static void tables_update(hdr_d3d12_t *e, ID3D12GraphicsCommandList *cmd)
{
   const uint16_t *cube;
   const uint16_t *aces2;
   const uint16_t *film;
   const uint16_t *desat;
   unsigned        gen = hdr_dt_generation();
   unsigned        i;

   if (!e->tables[T_DUMMY_2D])
   {
      e->tables[T_DUMMY_2D] = table_make(e, cmd, NULL, 1, 1, 1, 4, false, false);
      e->tables[T_DUMMY_3D] = table_make(e, cmd, NULL, 1, 1, 1, 4, false, true);
   }
   if (gen == e->generation)
      return;
   e->generation = gen;
   for (i = T_ACES2_LUT; i <= T_CUBE; i++)
   {
      retire(e, e->tables[i]);
      e->tables[i] = NULL;
   }
   cube  = hdr_dt_aces2_cube();
   aces2 = hdr_dt_aces2_lut();
   film  = hdr_dt_film_lut();
   desat = hdr_dt_film_desat();
   if (cube)
      e->tables[T_CUBE] = table_make(e, cmd, cube, HDR_DT_ACES2_CUBE, HDR_DT_ACES2_CUBE,
            HDR_DT_ACES2_CUBE, 4, true, true);
   if (aces2)
      e->tables[T_ACES2_LUT] = table_make(e, cmd, aces2, HDR_DT_ACES2_LUT_WIDTH, 1, 1, 4, false, false);
   if (film)
      e->tables[T_FILM_LUT] = table_make(e, cmd, film, HDR_DT_FILM_LUT_WIDTH, 1, 1, 1, false, false);
   if (desat)
      e->tables[T_DESAT] = table_make(e, cmd, desat, HDR_DT_DESAT_WIDTH, HDR_DT_DESAT_HEIGHT, 1, 4, false, false);
}

/* SSAA's samples at @place of the heap: @res, multisampled, or a null view of one */
static void write_srv_ms(hdr_d3d12_t *e, unsigned place, ID3D12Resource *res, DXGI_FORMAT format)
{
   D3D12_SHADER_RESOURCE_VIEW_DESC desc;
   D3D12_CPU_DESCRIPTOR_HANDLE     h = heap_cpu(e->srv_heap);
   h.ptr += (SIZE_T)place * e->srv_step;
   memset(&desc, 0, sizeof(desc));
   desc.Format                  = format;
   desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
   desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2DMS;
   ID3D12Device_CreateShaderResourceView(e->device, res, &desc, h);
}

static void write_srv(hdr_d3d12_t *e, unsigned place, ID3D12Resource *res,
      DXGI_FORMAT format, bool three_d)
{
   D3D12_SHADER_RESOURCE_VIEW_DESC desc;
   D3D12_CPU_DESCRIPTOR_HANDLE     h = heap_cpu(e->srv_heap);
   h.ptr += (SIZE_T)place * e->srv_step;
   memset(&desc, 0, sizeof(desc));
   desc.Format                  = format;
   desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
   if (three_d)
   {
      desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MipLevels = 1;
   }
   else
   {
      desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipLevels = 1;
   }
   ID3D12Device_CreateShaderResourceView(e->device, res, &desc, h);
}

static bool output_make(hdr_d3d12_t *e, unsigned index, unsigned w, unsigned h)
{
   D3D12_RESOURCE_DESC         desc;
   D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(e->rtv_heap);
   hdr_d3d12_output_t         *o   = &e->outputs[index];
   memset(&desc, 0, sizeof(desc));
   desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   desc.Width            = w;
   desc.Height           = h;
   desc.DepthOrArraySize = 1;
   desc.MipLevels        = 1;
   desc.Format           = e->format;
   desc.SampleDesc.Count = 1;
   desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
   if (!(o->texture = committed(e, D3D12_HEAP_TYPE_DEFAULT, &desc, e->handed)))
      return false;
   rtv.ptr += (SIZE_T)index * e->rtv_step;
   ID3D12Device_CreateRenderTargetView(e->device, o->texture, NULL, rtv);
   o->width  = w;
   o->height = h;
   return true;
}

/* @constants (b1) 32-bit values at the root, and a table of the transform's
 * constants (b0) and @srvs views from t0; with @post, @post_constants
 * more at b2 */
static bool make_root_with(hdr_d3d12_t *e, ID3D12RootSignature **out, unsigned constants,
      unsigned srvs, unsigned post_constants)
{
   D3D12_DESCRIPTOR_RANGE    ranges[2];
   D3D12_ROOT_PARAMETER      params[3];
   D3D12_STATIC_SAMPLER_DESC samplers[2];
   D3D12_ROOT_SIGNATURE_DESC desc;
   ID3DBlob                 *blob = NULL, *errors = NULL;
   HMODULE                   lib  = GetModuleHandleA("d3d12.dll");
   serialize_root_t          serialize;
   unsigned                  i;
   bool                      ok = false;

   if (!lib)
      return false;
   serialize = (serialize_root_t)GetProcAddress(lib, "D3D12SerializeRootSignature");
   if (!serialize)
      return false;
   memset(ranges, 0, sizeof(ranges));
   ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
   ranges[0].NumDescriptors                    = 1;
   ranges[0].BaseShaderRegister                = 0;
   ranges[0].OffsetInDescriptorsFromTableStart = 0;
   ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
   ranges[1].NumDescriptors                    = srvs;
   ranges[1].BaseShaderRegister                = 0;
   ranges[1].OffsetInDescriptorsFromTableStart = 1;
   memset(params, 0, sizeof(params));
   /* b1 the encode's constants; the slot's table: b0 the transform's, t0-t4
    * the picture and tables */
   params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   params[0].Constants.ShaderRegister  = 1;
   params[0].Constants.Num32BitValues  = constants;
   params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
   params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   params[1].DescriptorTable.NumDescriptorRanges = 2;
   params[1].DescriptorTable.pDescriptorRanges   = ranges;
   params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
   params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   params[2].Constants.ShaderRegister  = 2;
   params[2].Constants.Num32BitValues  = post_constants;
   params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
   memset(samplers, 0, sizeof(samplers));
   for (i = 0; i < 2; i++)
   {
      samplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      samplers[i].AddressU         = i ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      samplers[i].AddressV         = samplers[i].AddressU;
      samplers[i].AddressW         = samplers[i].AddressU;
      samplers[i].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
      samplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
      samplers[i].ShaderRegister   = i;
      samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
   }
   memset(&desc, 0, sizeof(desc));
   desc.NumParameters     = post_constants ? 3 : 2;
   desc.pParameters       = params;
   desc.NumStaticSamplers = 2;
   desc.pStaticSamplers   = samplers;
   if (SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
      ok = SUCCEEDED(ID3D12Device_CreateRootSignature(e->device, 0,
               ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
               &iid_root_signature, (void **)out));
   if (blob)
      ID3D10Blob_Release(blob);
   if (errors)
      ID3D10Blob_Release(errors);
   return ok;
}

static bool make_root(hdr_d3d12_t *e)
{
   return make_root_with(e, &e->root, sizeof(struct hdr_encode_params) / 4, HDR_D3D12_SRVS - 1, 0);
}

/* The triangle with @ps into a render target of @format, under @root;
 * with @add, added to what is there */
static bool make_pso_with(hdr_d3d12_t *e, ID3D12RootSignature *root, const unsigned char *ps,
      size_t ps_size, DXGI_FORMAT format, bool add, ID3D12PipelineState **out)
{
   D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
   memset(&desc, 0, sizeof(desc));
   desc.pRootSignature                          = root;
   desc.VS.pShaderBytecode                      = hdr_encode_dxbc_VS;
   desc.VS.BytecodeLength                       = sizeof(hdr_encode_dxbc_VS);
   desc.PS.pShaderBytecode                      = ps;
   desc.PS.BytecodeLength                       = ps_size;
   desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
   if (add)
   {
      desc.BlendState.RenderTarget[0].BlendEnable    = TRUE;
      desc.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
      desc.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_ONE;
      desc.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
      desc.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
      desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
      desc.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
   }
   desc.SampleMask                              = 0xffffffffu;
   desc.RasterizerState.FillMode                = D3D12_FILL_MODE_SOLID;
   desc.RasterizerState.CullMode                = D3D12_CULL_MODE_NONE;
   desc.RasterizerState.DepthClipEnable         = TRUE;
   desc.PrimitiveTopologyType                   = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   desc.NumRenderTargets                        = 1;
   desc.RTVFormats[0]                           = format;
   desc.SampleDesc.Count                        = 1;
   return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(e->device, &desc,
            &iid_pipeline_state, (void **)out));
}

static bool make_pso(hdr_d3d12_t *e, const unsigned char *ps, size_t ps_size, ID3D12PipelineState **out)
{
   return make_pso_with(e, e->root, ps, ps_size, e->format, false, out);
}

static ID3D12DescriptorHeap *make_heap(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
      UINT count, bool visible)
{
   D3D12_DESCRIPTOR_HEAP_DESC desc;
   ID3D12DescriptorHeap      *heap = NULL;
   memset(&desc, 0, sizeof(desc));
   desc.Type           = type;
   desc.NumDescriptors = count;
   desc.Flags          = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
   if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &desc, &iid_descriptor_heap, (void **)&heap)))
      return NULL;
   return heap;
}

/* ---- the post effects ---- */

/* The bloom's: its constants (b0) at the root, a table of its source (t0) */
static bool make_root_bloom(hdr_d3d12_t *e, ID3D12RootSignature **out)
{
   D3D12_DESCRIPTOR_RANGE    range;
   D3D12_ROOT_PARAMETER      params[2];
   D3D12_STATIC_SAMPLER_DESC sampler;
   D3D12_ROOT_SIGNATURE_DESC desc;
   ID3DBlob                 *blob = NULL, *errors = NULL;
   HMODULE                   lib  = GetModuleHandleA("d3d12.dll");
   serialize_root_t          serialize;
   bool                      ok = false;

   if (!lib || !(serialize = (serialize_root_t)GetProcAddress(lib, "D3D12SerializeRootSignature")))
      return false;
   memset(&range, 0, sizeof(range));
   range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
   range.NumDescriptors     = 1;
   memset(params, 0, sizeof(params));
   params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   params[0].Constants.ShaderRegister = 0;
   params[0].Constants.Num32BitValues = sizeof(struct hdr_bloom_params) / 4;
   params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
   params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   params[1].DescriptorTable.NumDescriptorRanges = 1;
   params[1].DescriptorTable.pDescriptorRanges   = &range;
   params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
   memset(&sampler, 0, sizeof(sampler));
   sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
   sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
   sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
   sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
   sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
   sampler.MaxLOD           = D3D12_FLOAT32_MAX;
   sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
   memset(&desc, 0, sizeof(desc));
   desc.NumParameters     = 2;
   desc.pParameters       = params;
   desc.NumStaticSamplers = 1;
   desc.pStaticSamplers   = &sampler;
   if (SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
      ok = SUCCEEDED(ID3D12Device_CreateRootSignature(e->device, 0,
               ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
               &iid_root_signature, (void **)out));
   if (blob)
      ID3D10Blob_Release(blob);
   if (errors)
      ID3D10Blob_Release(errors);
   return ok;
}

static const struct { const unsigned char *code; size_t size; } post_stages[HDR_BLOOM_STAGES] = {
   { hdr_encode_dxbc_BLOOM_BRIGHT, sizeof(hdr_encode_dxbc_BLOOM_BRIGHT) },
   { hdr_encode_dxbc_BLOOM_BLUR, sizeof(hdr_encode_dxbc_BLOOM_BLUR) },
   { hdr_encode_dxbc_BLOOM_DOWN, sizeof(hdr_encode_dxbc_BLOOM_DOWN) },
   { hdr_encode_dxbc_BLOOM_DOWN_KARIS, sizeof(hdr_encode_dxbc_BLOOM_DOWN_KARIS) },
   { hdr_encode_dxbc_BLOOM_UP, sizeof(hdr_encode_dxbc_BLOOM_UP) }
};

static DXGI_FORMAT post_dxgi(int k)
{
   return k == P_COMPACT ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

/* The roots, the composite, the heaps, the first time */
static bool post_make(hdr_d3d12_t *e)
{
   hdr_d3d12_post_t *p = &e->post;
   if (p->made)
      return p->composite != NULL;
   p->made = 1;
   if (     !make_root_bloom(e, &p->root_bloom)
         || !make_root_with(e, &p->root_composite, sizeof(struct hdr_encode_params) / 4, 7,
               sizeof(struct hdr_post_params) / 4)
         || !(p->srv_heap = make_heap(e->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
               HDR_D3D12_SLOTS * HDR_D3D12_POST_SRVS, true))
         || !(p->rtv_heap = make_heap(e->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 + HDR_POST_IMAGES, false)))
      return false;
   make_pso_with(e, p->root_composite, hdr_encode_dxbc_POST, sizeof(hdr_encode_dxbc_POST),
         e->format, false, &p->composite);
   return p->composite != NULL;
}

/* What draws intermediates of format @k, the first time */
static bool post_format_make(hdr_d3d12_t *e, int k)
{
   hdr_d3d12_post_t *p = &e->post;
   unsigned          i;
   if (p->format_made[k])
      return p->stage[k][HDR_BLOOM_UP] != NULL;
   p->format_made[k] = 1;
   if (     !make_pso_with(e, e->root, hdr_encode_dxbc_PS_LINEAR, sizeof(hdr_encode_dxbc_PS_LINEAR),
               post_dxgi(k), false, &p->linear[k])
         || !make_pso_with(e, e->root, hdr_encode_dxbc_PS_SSAA_LINEAR, sizeof(hdr_encode_dxbc_PS_SSAA_LINEAR),
               post_dxgi(k), false, &p->linear_ssaa[k])
         || !make_pso_with(e, e->root, hdr_encode_dxbc_PS_SUMS_LINEAR, sizeof(hdr_encode_dxbc_PS_SUMS_LINEAR),
               post_dxgi(k), false, &p->linear_sums[k]))
      return false;
   for (i = 0; i < HDR_BLOOM_STAGES; i++)
      if (!make_pso_with(e, p->root_bloom, post_stages[i].code, post_stages[i].size,
               post_dxgi(k), i == HDR_BLOOM_UP, &p->stage[k][i]))
         return false;
   return true;
}

static void post_free(hdr_d3d12_t *e)
{
   hdr_d3d12_post_t *p = &e->post;
   unsigned          i, k;
   RELEASE(ID3D12Resource, p->scene.texture);
   for (i = 0; i < HDR_POST_IMAGES; i++)
      RELEASE(ID3D12Resource, p->images[i].texture);
   for (k = 0; k < P_FORMATS; k++)
   {
      RELEASE(ID3D12PipelineState, p->linear[k]);
      RELEASE(ID3D12PipelineState, p->linear_ssaa[k]);
      RELEASE(ID3D12PipelineState, p->linear_sums[k]);
      for (i = 0; i < HDR_BLOOM_STAGES; i++)
         RELEASE(ID3D12PipelineState, p->stage[k][i]);
   }
   RELEASE(ID3D12PipelineState, p->composite);
   RELEASE(ID3D12DescriptorHeap, p->rtv_heap);
   RELEASE(ID3D12DescriptorHeap, p->srv_heap);
   RELEASE(ID3D12RootSignature, p->root_composite);
   RELEASE(ID3D12RootSignature, p->root_bloom);
}

/* Compact Intermediates where the device draws to, blends and filters
 * R11G11B10 */
static int post_format(hdr_d3d12_t *e)
{
   D3D12_FEATURE_DATA_FORMAT_SUPPORT support;
   const UINT need = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE
         | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
   if (!hdr_post_compact())
      return P_HALF;
   memset(&support, 0, sizeof(support));
   support.Format = DXGI_FORMAT_R11G11B10_FLOAT;
   if (     FAILED(ID3D12Device_CheckFeatureSupport(e->device, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
         || (support.Support1 & need) != need)
      return P_HALF;
   return P_COMPACT;
}

/* The intermediate at @place of the RTV heap (0 the scene) as @k, @w x @h:
 * made again, what it was retired with the slot, when it is not. Left in
 * the pixel shader resource state. */
static bool post_image(hdr_d3d12_t *e, hdr_d3d12_image_t *o, unsigned place, int k,
      unsigned w, unsigned h)
{
   D3D12_RESOURCE_DESC         desc;
   D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(e->post.rtv_heap);
   if (o->texture && o->width == w && o->height == h && o->format == k)
      return true;
   retire(e, o->texture);
   o->texture = NULL;
   memset(&desc, 0, sizeof(desc));
   desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   desc.Width            = w;
   desc.Height           = h;
   desc.DepthOrArraySize = 1;
   desc.MipLevels        = 1;
   desc.Format           = post_dxgi(k);
   desc.SampleDesc.Count = 1;
   desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
   if (!(o->texture = committed(e, D3D12_HEAP_TYPE_DEFAULT, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)))
      return false;
   rtv.ptr += (SIZE_T)place * e->rtv_step;
   ID3D12Device_CreateRenderTargetView(e->device, o->texture, NULL, rtv);
   o->width  = w;
   o->height = h;
   o->format = k;
   return true;
}

static bool post_ready(hdr_d3d12_t *e, const struct hdr_post_plan *plan, unsigned w, unsigned h, int *k)
{
   unsigned i;
   *k = post_format(e);
   if (!post_make(e) || !post_format_make(e, *k) || !post_image(e, &e->post.scene, 0, *k, w, h))
      return false;
   for (i = 0; i < HDR_POST_IMAGES; i++)
      if (plan->w[i] && !post_image(e, &e->post.images[i], 1 + i, *k, plan->w[i], plan->h[i]))
         return false;
   return true;
}

static hdr_d3d12_image_t *post_img(hdr_d3d12_t *e, int image)
{
   return image == HDR_POST_SCENE ? &e->post.scene : &e->post.images[image];
}

/* A view at @place of the post effects' heap */
static void post_srv(hdr_d3d12_t *e, unsigned place, ID3D12Resource *res, DXGI_FORMAT format,
      bool three_d, bool ms)
{
   D3D12_SHADER_RESOURCE_VIEW_DESC desc;
   D3D12_CPU_DESCRIPTOR_HANDLE     h = heap_cpu(e->post.srv_heap);
   h.ptr += (SIZE_T)place * e->srv_step;
   memset(&desc, 0, sizeof(desc));
   desc.Format                  = format;
   desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
   if (ms)
      desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2DMS;
   else if (three_d)
   {
      desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MipLevels = 1;
   }
   else
   {
      desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipLevels = 1;
   }
   ID3D12Device_CreateShaderResourceView(e->device, res, &desc, h);
}

static void post_cbv(hdr_d3d12_t *e, unsigned place)
{
   D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
   D3D12_CPU_DESCRIPTOR_HANDLE     h = heap_cpu(e->post.srv_heap);
   h.ptr             += (SIZE_T)place * e->srv_step;
   cbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(e->upload) + (UINT64)e->slot * HDR_D3D12_DT_STRIDE;
   cbv.SizeInBytes    = HDR_D3D12_DT_STRIDE;
   ID3D12Device_CreateConstantBufferView(e->device, &cbv, h);
}

static void post_tables(hdr_d3d12_t *e, unsigned place)
{
   unsigned i;
   for (i = 0; i < 4; i++)
   {
      unsigned t = i;
      if (!e->tables[t])
         t = (i == T_CUBE) ? T_DUMMY_3D : T_DUMMY_2D;
      post_srv(e, place + i, e->tables[t], DXGI_FORMAT_R16G16B16A16_FLOAT, i == T_CUBE, false);
   }
}

/* One full-target draw into @o, which is left shader readable */
static void post_draw(hdr_d3d12_t *e, ID3D12GraphicsCommandList *cmd, hdr_d3d12_image_t *o,
      unsigned rtv_place, unsigned w, unsigned h)
{
   D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(e->post.rtv_heap);
   D3D12_VIEWPORT              vp;
   D3D12_RECT                  scissor;
   rtv.ptr       += (SIZE_T)rtv_place * e->rtv_step;
   vp.TopLeftX    = 0.0f;
   vp.TopLeftY    = 0.0f;
   vp.Width       = (float)w;
   vp.Height      = (float)h;
   vp.MinDepth    = 0.0f;
   vp.MaxDepth    = 1.0f;
   scissor.left   = 0;
   scissor.top    = 0;
   scissor.right  = (LONG)w;
   scissor.bottom = (LONG)h;
   barrier(cmd, o->texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
   ID3D12GraphicsCommandList_RSSetViewports(cmd, 1, &vp);
   ID3D12GraphicsCommandList_RSSetScissorRects(cmd, 1, &scissor);
   ID3D12GraphicsCommandList_OMSetRenderTargets(cmd, 1, &rtv, FALSE, NULL);
   ID3D12GraphicsCommandList_DrawInstanced(cmd, 3, 1, 0, 0);
   barrier(cmd, o->texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static D3D12_GPU_DESCRIPTOR_HANDLE post_table(hdr_d3d12_t *e, unsigned place)
{
   D3D12_GPU_DESCRIPTOR_HANDLE t = heap_gpu(e->post.srv_heap);
   t.ptr += (UINT64)place * e->srv_step;
   return t;
}

/* The picture through the post effects into the output @o (at @rtv, in
 * the render target state): the slot's constants written, @params the
 * encode's */
static void post_encode(hdr_d3d12_t *e, ID3D12GraphicsCommandList *cmd,
      const struct hdr_post_plan *plan, int k, const struct hdr_encode_params *params,
      ID3D12Resource *picture, DXGI_FORMAT picture_format, ID3D12Resource *samples,
      ID3D12Resource *sums, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned w, unsigned h)
{
   hdr_d3d12_post_t     *p     = &e->post;
   const unsigned        first = e->slot * HDR_D3D12_POST_SRVS;
   ID3D12DescriptorHeap *heaps[1];
   D3D12_VIEWPORT        vp;
   D3D12_RECT            scissor;
   int                   i;

   /* the descriptors: the encode's, the composite's, each pass's source */
   post_cbv(e, first);
   post_srv(e, first + 1, picture, picture_format, false, false);
   post_tables(e, first + 2);
   if (sums)
      post_srv(e, first + 6, sums, DXGI_FORMAT_R16G16B16A16_UINT, false, false);
   else
      post_srv(e, first + 6, params->ssaa_log2 ? samples : NULL, picture_format, false, true);
   post_cbv(e, first + HDR_D3D12_POST_COMPOSITE);
   post_srv(e, first + HDR_D3D12_POST_COMPOSITE + 1, p->scene.texture, post_dxgi(k), false, false);
   post_tables(e, first + HDR_D3D12_POST_COMPOSITE + 2);
   post_srv(e, first + HDR_D3D12_POST_COMPOSITE + 6, post_img(e, plan->band_t)->texture, post_dxgi(k), false, false);
   post_srv(e, first + HDR_D3D12_POST_COMPOSITE + 7, post_img(e, plan->band_w)->texture, post_dxgi(k), false, false);
   for (i = 0; i < plan->passes; i++)
      post_srv(e, first + HDR_D3D12_POST_BLOOM + i, post_img(e, plan->pass[i].src)->texture,
            post_dxgi(k), false, false);
   heaps[0] = p->srv_heap;
   ID3D12GraphicsCommandList_SetDescriptorHeaps(cmd, 1, heaps);
   ID3D12GraphicsCommandList_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

   /* the picture into scene-linear light */
   ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, e->root);
   ID3D12GraphicsCommandList_SetPipelineState(cmd, sums ? p->linear_sums[k]
         : params->ssaa_log2 ? p->linear_ssaa[k] : p->linear[k]);
   ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, 0, sizeof(*params) / 4, params, 0);
   ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 1, post_table(e, first));
   post_draw(e, cmd, &p->scene, 0, w, h);
   /* the bloom */
   ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, p->root_bloom);
   for (i = 0; i < plan->passes; i++)
   {
      const struct hdr_post_pass *q = &plan->pass[i];
      ID3D12GraphicsCommandList_SetPipelineState(cmd, p->stage[k][q->stage]);
      ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, 0, sizeof(q->params) / 4, &q->params, 0);
      ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 1, post_table(e, first + HDR_D3D12_POST_BLOOM + i));
      post_draw(e, cmd, &p->images[q->dst], 1 + q->dst, plan->w[q->dst], plan->h[q->dst]);
   }
   /* the composite, into the output */
   vp.TopLeftX    = 0.0f;
   vp.TopLeftY    = 0.0f;
   vp.Width       = (float)w;
   vp.Height      = (float)h;
   vp.MinDepth    = 0.0f;
   vp.MaxDepth    = 1.0f;
   scissor.left   = 0;
   scissor.top    = 0;
   scissor.right  = (LONG)w;
   scissor.bottom = (LONG)h;
   ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, p->root_composite);
   ID3D12GraphicsCommandList_SetPipelineState(cmd, p->composite);
   ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, 0, sizeof(*params) / 4, params, 0);
   ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 1, post_table(e, first + HDR_D3D12_POST_COMPOSITE));
   ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, 2, sizeof(plan->composite) / 4, &plan->composite, 0);
   ID3D12GraphicsCommandList_RSSetViewports(cmd, 1, &vp);
   ID3D12GraphicsCommandList_RSSetScissorRects(cmd, 1, &scissor);
   ID3D12GraphicsCommandList_OMSetRenderTargets(cmd, 1, &rtv, FALSE, NULL);
   ID3D12GraphicsCommandList_DrawInstanced(cmd, 3, 1, 0, 0);
}

hdr_d3d12_t *hdr_d3d12_new(ID3D12Device *device, DXGI_FORMAT format,
      D3D12_RESOURCE_STATES handed, ID3D12Fence *fence)
{
   D3D12_FEATURE_DATA_FORMAT_SUPPORT support;
   D3D12_RANGE                       none;
   hdr_d3d12_t                      *e;

   memset(&support, 0, sizeof(support));
   support.Format = format;
   if (     FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
         || !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
      return NULL;
   e = (hdr_d3d12_t*)calloc(1, sizeof(*e));
   if (!e)
      return NULL;
   e->device     = device;
   ID3D12Device_AddRef(device);
   e->fence      = fence;
   ID3D12Fence_AddRef(fence);
   e->format     = format;
   e->handed     = handed;
   e->generation = hdr_dt_generation() - 1;
   e->srv_step   = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   e->rtv_step   = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
   none.Begin    = 0;
   none.End      = 0;
   if (     !make_root(e)
         || !make_pso(e, hdr_encode_dxbc_PS, sizeof(hdr_encode_dxbc_PS), &e->pso)
         || !make_pso(e, hdr_encode_dxbc_PS_SSAA, sizeof(hdr_encode_dxbc_PS_SSAA), &e->pso_ssaa)
         || !make_pso(e, hdr_encode_dxbc_PS_SUMS, sizeof(hdr_encode_dxbc_PS_SUMS), &e->pso_sums)
         || !(e->srv_heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
               HDR_D3D12_SLOTS * HDR_D3D12_SRVS, true))
         || !(e->rtv_heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, HDR_D3D12_MAX_OUTPUTS, false))
         || !(e->upload = buffer_upload(e, (UINT64)HDR_D3D12_SLOTS * HDR_D3D12_DT_STRIDE))
         || FAILED(ID3D12Resource_Map(e->upload, 0, &none, (void **)&e->upload_map)))
   {
      hdr_d3d12_free(e);
      return NULL;
   }
   return e;
}

void hdr_d3d12_free(hdr_d3d12_t *e)
{
   unsigned i;
   if (!e)
      return;
   for (i = 0; i < HDR_D3D12_SLOTS; i++)
      slot_release(&e->slots[i]);
   post_free(e);
   for (i = 0; i < HDR_D3D12_MAX_OUTPUTS; i++)
      RELEASE(ID3D12Resource, e->outputs[i].texture);
   for (i = 0; i < T_COUNT; i++)
      RELEASE(ID3D12Resource, e->tables[i]);
   if (e->upload && e->upload_map)
      ID3D12Resource_Unmap(e->upload, 0, NULL);
   RELEASE(ID3D12Resource, e->upload);
   RELEASE(ID3D12DescriptorHeap, e->rtv_heap);
   RELEASE(ID3D12DescriptorHeap, e->srv_heap);
   RELEASE(ID3D12PipelineState, e->pso);
   RELEASE(ID3D12PipelineState, e->pso_ssaa);
   RELEASE(ID3D12PipelineState, e->pso_sums);
   RELEASE(ID3D12RootSignature, e->root);
   RELEASE(ID3D12Fence, e->fence);
   RELEASE(ID3D12Device, e->device);
   free(e);
}

bool hdr_d3d12_encode(hdr_d3d12_t *e, ID3D12GraphicsCommandList *cmd,
      const struct hdr_output *out, ID3D12Resource *picture,
      DXGI_FORMAT picture_format, unsigned width, unsigned height,
      bool unorm8, ID3D12Resource *samples, unsigned samples_log2,
      ID3D12Resource *sums, unsigned index, uint64_t signal, ID3D12Resource **texture)
{
   hdr_d3d12_slot_t           *s;
   hdr_d3d12_output_t         *o;
   struct hdr_dt_block        *block;
   struct hdr_encode_params    params;
   D3D12_GPU_DESCRIPTOR_HANDLE table;
   D3D12_CPU_DESCRIPTOR_HANDLE rtv;
   D3D12_VIEWPORT              vp;
   D3D12_RECT                  scissor;
   ID3D12DescriptorHeap       *heaps[1];
   unsigned                    first, i;

   if (!e || !picture || width == 0 || height == 0 || index >= HDR_D3D12_MAX_OUTPUTS)
      return false;

   /* the slot comes round: longer ago than the core's command allocators
    * do, so this is not a wait but where the past goes */
   e->slot = (e->slot + 1) % HDR_D3D12_SLOTS;
   s       = &e->slots[e->slot];
   if (s->signal > ID3D12Fence_GetCompletedValue(e->fence))
      return false;
   slot_release(s);

   o = &e->outputs[index];
   if (o->texture && (o->width != width || o->height != height))
   {
      retire(e, o->texture);
      o->texture = NULL;
   }
   if (!o->texture && !output_make(e, index, width, height))
      return false;

   block = (struct hdr_dt_block*)(e->upload_map + (size_t)e->slot * HDR_D3D12_DT_STRIDE);
   hdr_dt_fill(block, out->paper_white_nits, out->max_nits,
         out->panel_peak_nits, out->panel_xy);
   tables_update(e, cmd);
   /* the cube only once the texture of it is here, whatever the block says */
   if (!e->tables[T_CUBE])
      block->gm[2] = 0.0f;
   /* (the samples' exact average is not 8 bits a channel: no dither) */
   if (sums)
      hdr_encode_params_fill(&params, out, false);
   else if (samples && samples_log2)
   {
      hdr_encode_params_fill(&params, out, false);
      params.ssaa_log2 = (int32_t)samples_log2;
   }
   else
      hdr_encode_params_fill(&params, out, unorm8);

   first = e->slot * HDR_D3D12_SRVS;
   {
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
      D3D12_CPU_DESCRIPTOR_HANDLE     h = heap_cpu(e->srv_heap);
      h.ptr          += (SIZE_T)first * e->srv_step;
      cbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(e->upload) + (UINT64)e->slot * HDR_D3D12_DT_STRIDE;
      cbv.SizeInBytes    = HDR_D3D12_DT_STRIDE;
      ID3D12Device_CreateConstantBufferView(e->device, &cbv, h);
   }
   write_srv(e, first + 1, picture, picture_format, false);
   if (sums)
      write_srv(e, first + 6, sums, DXGI_FORMAT_R16G16B16A16_UINT, false);
   else
      write_srv_ms(e, first + 6, params.ssaa_log2 ? samples : NULL, picture_format);
   for (i = 0; i < 4; i++)
   {
      unsigned t = i;
      if (!e->tables[t])
         t = (i == T_CUBE) ? T_DUMMY_3D : T_DUMMY_2D;
      write_srv(e, first + 2 + i, e->tables[t], DXGI_FORMAT_R16G16B16A16_FLOAT, i == T_CUBE);
   }
   table      = heap_gpu(e->srv_heap);
   table.ptr += (UINT64)first * e->srv_step;
   rtv        = heap_cpu(e->rtv_heap);
   rtv.ptr   += (SIZE_T)index * e->rtv_step;
   vp.TopLeftX      = 0.0f;
   vp.TopLeftY      = 0.0f;
   vp.Width         = (float)width;
   vp.Height        = (float)height;
   vp.MinDepth      = 0.0f;
   vp.MaxDepth      = 1.0f;
   scissor.left     = 0;
   scissor.top      = 0;
   scissor.right    = (LONG)width;
   scissor.bottom   = (LONG)height;
   heaps[0]         = e->srv_heap;

   barrier(cmd, o->texture, e->handed, D3D12_RESOURCE_STATE_RENDER_TARGET);
   if (hdr_post_enabled())
   {
      struct hdr_post_plan plan;
      int                  k;
      hdr_post_plan_make(&plan, width, height);
      /* (what cannot be made: the picture encoded without them) */
      if (post_ready(e, &plan, width, height, &k))
      {
         post_encode(e, cmd, &plan, k, &params, picture, picture_format, samples, sums, rtv, width, height);
         barrier(cmd, o->texture, D3D12_RESOURCE_STATE_RENDER_TARGET, e->handed);
         s->signal = signal;
         *texture = o->texture;
         return true;
      }
   }
   ID3D12GraphicsCommandList_SetDescriptorHeaps(cmd, 1, heaps);
   ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, e->root);
   ID3D12GraphicsCommandList_SetPipelineState(cmd, sums ? e->pso_sums : params.ssaa_log2 ? e->pso_ssaa : e->pso);
   ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, 0,
         sizeof(params) / 4, &params, 0);
   ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 1, table);
   ID3D12GraphicsCommandList_RSSetViewports(cmd, 1, &vp);
   ID3D12GraphicsCommandList_RSSetScissorRects(cmd, 1, &scissor);
   ID3D12GraphicsCommandList_OMSetRenderTargets(cmd, 1, &rtv, FALSE, NULL);
   ID3D12GraphicsCommandList_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ID3D12GraphicsCommandList_DrawInstanced(cmd, 3, 1, 0, 0);
   barrier(cmd, o->texture, D3D12_RESOURCE_STATE_RENDER_TARGET, e->handed);
   s->signal = signal;

   *texture = o->texture;
   return true;
}
