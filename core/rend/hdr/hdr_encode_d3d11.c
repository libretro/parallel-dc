/* See hdr_encode_d3d11.h. C89. */
#define COBJMACROS
#include <stdlib.h>
#include <string.h>

#include "hdr_encode_d3d11.h"
#include "hdr_output.h"
#include "hdr_transform.h"
#include "hdr_encode_dxbc.inc"

#define HDR_D3D11_MAX_OUTPUTS 32

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

/* The post effects' intermediates and what draws them, made the first
 * time they are on (hdr_output.h) */
typedef struct hdr_d3d11_image
{
   ID3D11Texture2D          *texture;
   ID3D11ShaderResourceView *view;
   ID3D11RenderTargetView   *rtv;
   unsigned                  width;
   unsigned                  height;
   DXGI_FORMAT               format;
} hdr_d3d11_image_t;

typedef struct hdr_d3d11_post
{
   int                   made;
   ID3D11PixelShader    *linear;
   ID3D11PixelShader    *composite;
   ID3D11PixelShader    *stage[HDR_BLOOM_STAGES];
   ID3D11BlendState     *add;
   ID3D11Buffer         *post;      /* hdr_post_params */
   ID3D11Buffer         *bloom;     /* hdr_bloom_params */
   hdr_d3d11_image_t     scene;
   hdr_d3d11_image_t     images[HDR_POST_IMAGES];
} hdr_d3d11_post_t;

typedef struct hdr_d3d11_output
{
   ID3D11Texture2D          *texture;
   ID3D11ShaderResourceView *view;
   ID3D11RenderTargetView   *rtv;
   unsigned                  width;
   unsigned                  height;
} hdr_d3d11_output_t;

struct hdr_d3d11
{
   ID3D11Device             *device;
   DXGI_FORMAT               format;
   ID3D11VertexShader       *vs;
   ID3D11PixelShader        *ps;
   ID3D11Buffer             *dt;       /* hdr_dt_block */
   ID3D11Buffer             *params;   /* hdr_encode_params */
   ID3D11SamplerState       *linear_clamp;
   ID3D11SamplerState       *linear_wrap;
   ID3D11RasterizerState    *raster;
   ID3D11ShaderResourceView *tables[T_COUNT];
   unsigned                  generation;
   hdr_d3d11_output_t        outputs[HDR_D3D11_MAX_OUTPUTS];
   hdr_d3d11_post_t          post;
};

#define RELEASE(type, p) do { if (p) { type##_Release(p); (p) = NULL; } } while (0)

static void output_release(hdr_d3d11_output_t *o)
{
   RELEASE(ID3D11RenderTargetView, o->rtv);
   RELEASE(ID3D11ShaderResourceView, o->view);
   RELEASE(ID3D11Texture2D, o->texture);
   o->width  = 0;
   o->height = 0;
}

/* A table as an RGBA16F texture, half floats being the 16-bit format
 * every device filters: @data unorm16 of @channels a texel (or half RGBA
 * when @half), NULL for a texel of zero; 3D or 2D. Its view, or NULL. */
static ID3D11ShaderResourceView *table_make(hdr_d3d11_t *e, const uint16_t *data,
      unsigned w, unsigned h, unsigned d, unsigned channels, bool half, bool three_d)
{
   size_t                    count = (size_t)w * h * d, i;
   unsigned                  c;
   uint16_t                 *texels = (uint16_t*)calloc(count * 4, sizeof(uint16_t));
   D3D11_SUBRESOURCE_DATA    init;
   ID3D11Resource           *res  = NULL;
   ID3D11ShaderResourceView *view = NULL;
   HRESULT                   hr;

   if (!texels)
      return NULL;
   if (data && half)
      memcpy(texels, data, count * 4 * sizeof(uint16_t));
   else if (data)
      for (i = 0; i < count; i++)
         for (c = 0; c < 4; c++)
            texels[i * 4 + c] = hdr_dt_unorm16_to_half(data[i * channels + (c < channels ? c : 0)]);
   init.pSysMem          = texels;
   init.SysMemPitch      = w * 8;
   init.SysMemSlicePitch = w * h * 8;
   if (three_d)
   {
      D3D11_TEXTURE3D_DESC desc;
      memset(&desc, 0, sizeof(desc));
      desc.Width     = w;
      desc.Height    = h;
      desc.Depth     = d;
      desc.MipLevels = 1;
      desc.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
      desc.Usage     = D3D11_USAGE_IMMUTABLE;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      hr = ID3D11Device_CreateTexture3D(e->device, &desc, &init, (ID3D11Texture3D**)&res);
   }
   else
   {
      D3D11_TEXTURE2D_DESC desc;
      memset(&desc, 0, sizeof(desc));
      desc.Width            = w;
      desc.Height           = h;
      desc.MipLevels        = 1;
      desc.ArraySize        = 1;
      desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
      desc.SampleDesc.Count = 1;
      desc.Usage            = D3D11_USAGE_IMMUTABLE;
      desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
      hr = ID3D11Device_CreateTexture2D(e->device, &desc, &init, (ID3D11Texture2D**)&res);
   }
   free(texels);
   if (FAILED(hr))
      return NULL;
   if (FAILED(ID3D11Device_CreateShaderResourceView(e->device, res, NULL, &view)))
      view = NULL;
   ID3D11Resource_Release(res);
   return view;
}

/* The tables again when hdr_transform.c has new ones: a table it does not
 * have is left out, and the shader reads the stand-in. */
static void tables_update(hdr_d3d11_t *e)
{
   const uint16_t *cube;
   const uint16_t *aces2;
   const uint16_t *film;
   const uint16_t *desat;
   unsigned        gen = hdr_dt_generation();
   unsigned        i;

   if (!e->tables[T_DUMMY_2D])
   {
      e->tables[T_DUMMY_2D] = table_make(e, NULL, 1, 1, 1, 4, false, false);
      e->tables[T_DUMMY_3D] = table_make(e, NULL, 1, 1, 1, 4, false, true);
   }
   if (gen == e->generation)
      return;
   e->generation = gen;
   for (i = T_ACES2_LUT; i <= T_CUBE; i++)
      RELEASE(ID3D11ShaderResourceView, e->tables[i]);
   cube  = hdr_dt_aces2_cube();
   aces2 = hdr_dt_aces2_lut();
   film  = hdr_dt_film_lut();
   desat = hdr_dt_film_desat();
   if (cube)
      e->tables[T_CUBE] = table_make(e, cube, HDR_DT_ACES2_CUBE, HDR_DT_ACES2_CUBE,
            HDR_DT_ACES2_CUBE, 4, true, true);
   if (aces2)
      e->tables[T_ACES2_LUT] = table_make(e, aces2, HDR_DT_ACES2_LUT_WIDTH, 1, 1, 4, false, false);
   if (film)
      e->tables[T_FILM_LUT] = table_make(e, film, HDR_DT_FILM_LUT_WIDTH, 1, 1, 1, false, false);
   if (desat)
      e->tables[T_DESAT] = table_make(e, desat, HDR_DT_DESAT_WIDTH, HDR_DT_DESAT_HEIGHT, 1, 4, false, false);
}

static ID3D11ShaderResourceView *table_view(hdr_d3d11_t *e, unsigned t)
{
   if (e->tables[t])
      return e->tables[t];
   return e->tables[t == T_CUBE ? T_DUMMY_3D : T_DUMMY_2D];
}

static bool output_make(hdr_d3d11_t *e, hdr_d3d11_output_t *o, unsigned w, unsigned h)
{
   D3D11_TEXTURE2D_DESC desc;
   memset(&desc, 0, sizeof(desc));
   desc.Width            = w;
   desc.Height           = h;
   desc.MipLevels        = 1;
   desc.ArraySize        = 1;
   desc.Format           = e->format;
   desc.SampleDesc.Count = 1;
   desc.Usage            = D3D11_USAGE_DEFAULT;
   desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
   if (     FAILED(ID3D11Device_CreateTexture2D(e->device, &desc, NULL, &o->texture))
         || FAILED(ID3D11Device_CreateShaderResourceView(e->device, (ID3D11Resource*)o->texture, NULL, &o->view))
         || FAILED(ID3D11Device_CreateRenderTargetView(e->device, (ID3D11Resource*)o->texture, NULL, &o->rtv)))
   {
      output_release(o);
      return false;
   }
   o->width  = w;
   o->height = h;
   return true;
}

static ID3D11Buffer *constants(ID3D11Device *device, unsigned size)
{
   D3D11_BUFFER_DESC desc;
   ID3D11Buffer     *b = NULL;
   memset(&desc, 0, sizeof(desc));
   desc.ByteWidth      = size;
   desc.Usage          = D3D11_USAGE_DYNAMIC;
   desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   if (FAILED(ID3D11Device_CreateBuffer(device, &desc, NULL, &b)))
      return NULL;
   return b;
}

static ID3D11SamplerState *sampler(ID3D11Device *device, D3D11_TEXTURE_ADDRESS_MODE mode)
{
   D3D11_SAMPLER_DESC  desc;
   ID3D11SamplerState *s = NULL;
   memset(&desc, 0, sizeof(desc));
   desc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
   desc.AddressU       = mode;
   desc.AddressV       = mode;
   desc.AddressW       = mode;
   desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
   desc.MaxLOD         = D3D11_FLOAT32_MAX;
   if (FAILED(ID3D11Device_CreateSamplerState(device, &desc, &s)))
      return NULL;
   return s;
}

hdr_d3d11_t *hdr_d3d11_new(ID3D11Device *device, DXGI_FORMAT format)
{
   D3D11_RASTERIZER_DESC raster;
   UINT                  support = 0;
   hdr_d3d11_t          *e;

   if (ID3D11Device_GetFeatureLevel(device) < D3D_FEATURE_LEVEL_11_0)
      return NULL;
   if (     FAILED(ID3D11Device_CheckFormatSupport(device, format, &support))
         || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
         || !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
      return NULL;
   e = (hdr_d3d11_t*)calloc(1, sizeof(*e));
   if (!e)
      return NULL;
   e->device     = device;
   ID3D11Device_AddRef(device);
   e->format     = format;
   e->generation = hdr_dt_generation() - 1;
   memset(&raster, 0, sizeof(raster));
   raster.FillMode        = D3D11_FILL_SOLID;
   raster.CullMode        = D3D11_CULL_NONE;
   raster.DepthClipEnable = TRUE;
   if (     FAILED(ID3D11Device_CreateVertexShader(device, hdr_encode_dxbc_VS, sizeof(hdr_encode_dxbc_VS), NULL, &e->vs))
         || FAILED(ID3D11Device_CreatePixelShader(device, hdr_encode_dxbc_PS, sizeof(hdr_encode_dxbc_PS), NULL, &e->ps))
         || FAILED(ID3D11Device_CreateRasterizerState(device, &raster, &e->raster))
         || !(e->dt           = constants(device, sizeof(struct hdr_dt_block)))
         || !(e->params       = constants(device, sizeof(struct hdr_encode_params)))
         || !(e->linear_clamp = sampler(device, D3D11_TEXTURE_ADDRESS_CLAMP))
         || !(e->linear_wrap  = sampler(device, D3D11_TEXTURE_ADDRESS_WRAP)))
   {
      hdr_d3d11_free(e);
      return NULL;
   }
   return e;
}

static void image_release(hdr_d3d11_image_t *o)
{
   RELEASE(ID3D11RenderTargetView, o->rtv);
   RELEASE(ID3D11ShaderResourceView, o->view);
   RELEASE(ID3D11Texture2D, o->texture);
   o->width  = 0;
   o->height = 0;
}

static void post_free(hdr_d3d11_t *e)
{
   hdr_d3d11_post_t *p = &e->post;
   unsigned          i;
   image_release(&p->scene);
   for (i = 0; i < HDR_POST_IMAGES; i++)
      image_release(&p->images[i]);
   for (i = 0; i < HDR_BLOOM_STAGES; i++)
      RELEASE(ID3D11PixelShader, p->stage[i]);
   RELEASE(ID3D11PixelShader, p->composite);
   RELEASE(ID3D11PixelShader, p->linear);
   RELEASE(ID3D11BlendState, p->add);
   RELEASE(ID3D11Buffer, p->bloom);
   RELEASE(ID3D11Buffer, p->post);
}

void hdr_d3d11_free(hdr_d3d11_t *e)
{
   unsigned i;
   if (!e)
      return;
   post_free(e);
   for (i = 0; i < HDR_D3D11_MAX_OUTPUTS; i++)
      output_release(&e->outputs[i]);
   for (i = 0; i < T_COUNT; i++)
      RELEASE(ID3D11ShaderResourceView, e->tables[i]);
   RELEASE(ID3D11SamplerState, e->linear_wrap);
   RELEASE(ID3D11SamplerState, e->linear_clamp);
   RELEASE(ID3D11Buffer, e->params);
   RELEASE(ID3D11Buffer, e->dt);
   RELEASE(ID3D11RasterizerState, e->raster);
   RELEASE(ID3D11PixelShader, e->ps);
   RELEASE(ID3D11VertexShader, e->vs);
   RELEASE(ID3D11Device, e->device);
   free(e);
}

/* ---- the post effects ---- */

static bool post_make(hdr_d3d11_t *e)
{
   static const struct { const unsigned char *code; size_t size; } stages[HDR_BLOOM_STAGES] = {
      { hdr_encode_dxbc_BLOOM_BRIGHT, sizeof(hdr_encode_dxbc_BLOOM_BRIGHT) },
      { hdr_encode_dxbc_BLOOM_BLUR, sizeof(hdr_encode_dxbc_BLOOM_BLUR) },
      { hdr_encode_dxbc_BLOOM_DOWN, sizeof(hdr_encode_dxbc_BLOOM_DOWN) },
      { hdr_encode_dxbc_BLOOM_DOWN_KARIS, sizeof(hdr_encode_dxbc_BLOOM_DOWN_KARIS) },
      { hdr_encode_dxbc_BLOOM_UP, sizeof(hdr_encode_dxbc_BLOOM_UP) }
   };
   hdr_d3d11_post_t *p = &e->post;
   D3D11_BLEND_DESC  blend;
   unsigned          i;
   if (p->made)
      return p->post != NULL;
   p->made = 1;
   memset(&blend, 0, sizeof(blend));
   blend.RenderTarget[0].BlendEnable           = TRUE;
   blend.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
   blend.RenderTarget[0].DestBlend             = D3D11_BLEND_ONE;
   blend.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
   blend.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
   blend.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
   blend.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
   blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
   if (     FAILED(ID3D11Device_CreatePixelShader(e->device, hdr_encode_dxbc_PS_LINEAR, sizeof(hdr_encode_dxbc_PS_LINEAR), NULL, &p->linear))
         || FAILED(ID3D11Device_CreatePixelShader(e->device, hdr_encode_dxbc_POST, sizeof(hdr_encode_dxbc_POST), NULL, &p->composite))
         || FAILED(ID3D11Device_CreateBlendState(e->device, &blend, &p->add)))
      return false;
   for (i = 0; i < HDR_BLOOM_STAGES; i++)
      if (FAILED(ID3D11Device_CreatePixelShader(e->device, stages[i].code, stages[i].size, NULL, &p->stage[i])))
         return false;
   if (!(p->bloom = constants(e->device, sizeof(struct hdr_bloom_params))))
      return false;
   p->post = constants(e->device, sizeof(struct hdr_post_params));
   return p->post != NULL;
}

/* Compact Intermediates where the device draws to, blends and filters
 * R11G11B10 */
static DXGI_FORMAT post_format(hdr_d3d11_t *e)
{
   UINT support = 0;
   const UINT need = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE
         | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
   if (     hdr_post_compact()
         && SUCCEEDED(ID3D11Device_CheckFormatSupport(e->device, DXGI_FORMAT_R11G11B10_FLOAT, &support))
         && (support & need) == need)
      return DXGI_FORMAT_R11G11B10_FLOAT;
   return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static bool post_image(hdr_d3d11_t *e, hdr_d3d11_image_t *o, DXGI_FORMAT format, unsigned w, unsigned h)
{
   D3D11_TEXTURE2D_DESC desc;
   if (o->texture && o->width == w && o->height == h && o->format == format)
      return true;
   image_release(o);
   memset(&desc, 0, sizeof(desc));
   desc.Width            = w;
   desc.Height           = h;
   desc.MipLevels        = 1;
   desc.ArraySize        = 1;
   desc.Format           = format;
   desc.SampleDesc.Count = 1;
   desc.Usage            = D3D11_USAGE_DEFAULT;
   desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
   if (     FAILED(ID3D11Device_CreateTexture2D(e->device, &desc, NULL, &o->texture))
         || FAILED(ID3D11Device_CreateShaderResourceView(e->device, (ID3D11Resource*)o->texture, NULL, &o->view))
         || FAILED(ID3D11Device_CreateRenderTargetView(e->device, (ID3D11Resource*)o->texture, NULL, &o->rtv)))
   {
      image_release(o);
      return false;
   }
   o->width  = w;
   o->height = h;
   o->format = format;
   return true;
}

static bool post_ready(hdr_d3d11_t *e, const struct hdr_post_plan *plan, unsigned w, unsigned h)
{
   const DXGI_FORMAT format = post_format(e);
   unsigned          i;
   if (!post_make(e) || !post_image(e, &e->post.scene, format, w, h))
      return false;
   for (i = 0; i < HDR_POST_IMAGES; i++)
      if (plan->w[i] && !post_image(e, &e->post.images[i], format, plan->w[i], plan->h[i]))
         return false;
   return true;
}

static ID3D11ShaderResourceView *post_view(hdr_d3d11_t *e, int image)
{
   return image == HDR_POST_SCENE ? e->post.scene.view : e->post.images[image].view;
}

static void viewport(ID3D11DeviceContext *context, unsigned w, unsigned h)
{
   D3D11_VIEWPORT vp;
   vp.TopLeftX = 0.0f;
   vp.TopLeftY = 0.0f;
   vp.Width    = (float)w;
   vp.Height   = (float)h;
   vp.MinDepth = 0.0f;
   vp.MaxDepth = 1.0f;
   ID3D11DeviceContext_RSSetViewports(context, 1, &vp);
}

static bool upload(ID3D11DeviceContext *context, ID3D11Buffer *b, const void *data, size_t size)
{
   D3D11_MAPPED_SUBRESOURCE map;
   if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource*)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      return false;
   memcpy(map.pData, data, size);
   ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)b, 0);
   return true;
}

/* The picture (bound as the encode binds it, the state set) through the
 * post effects into @o */
static void post_encode(hdr_d3d11_t *e, ID3D11DeviceContext *context,
      const struct hdr_post_plan *plan, hdr_d3d11_output_t *o, unsigned w, unsigned h)
{
   hdr_d3d11_post_t         *p = &e->post;
   ID3D11ShaderResourceView *none[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
   ID3D11ShaderResourceView *srvs[7];
   ID3D11Buffer             *cbs[3];
   int                       i;

   /* the picture into scene-linear light */
   ID3D11DeviceContext_OMSetRenderTargets(context, 1, &p->scene.rtv, NULL);
   viewport(context, w, h);
   ID3D11DeviceContext_PSSetShader(context, p->linear, NULL, 0);
   ID3D11DeviceContext_Draw(context, 3, 0);
   ID3D11DeviceContext_PSSetShaderResources(context, 0, 7, none);
   /* the bloom */
   ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &p->bloom);
   for (i = 0; i < plan->passes; i++)
   {
      const struct hdr_post_pass *q   = &plan->pass[i];
      ID3D11ShaderResourceView   *src = post_view(e, q->src);
      if (!upload(context, p->bloom, &q->params, sizeof(q->params)))
         return;
      ID3D11DeviceContext_OMSetRenderTargets(context, 1, &p->images[q->dst].rtv, NULL);
      ID3D11DeviceContext_OMSetBlendState(context, q->stage == HDR_BLOOM_UP ? p->add : NULL, NULL, 0xffffffffu);
      viewport(context, plan->w[q->dst], plan->h[q->dst]);
      ID3D11DeviceContext_PSSetShader(context, p->stage[q->stage], NULL, 0);
      ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &src);
      ID3D11DeviceContext_Draw(context, 3, 0);
      ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, none);
   }
   ID3D11DeviceContext_OMSetBlendState(context, NULL, NULL, 0xffffffffu);
   /* the composite, into the output */
   if (!upload(context, p->post, &plan->composite, sizeof(plan->composite)))
      return;
   srvs[0] = p->scene.view;
   srvs[1] = table_view(e, T_ACES2_LUT);
   srvs[2] = table_view(e, T_FILM_LUT);
   srvs[3] = table_view(e, T_DESAT);
   srvs[4] = table_view(e, T_CUBE);
   srvs[5] = post_view(e, plan->band_t);
   srvs[6] = post_view(e, plan->band_w);
   cbs[0]  = e->dt;
   cbs[1]  = e->params;
   cbs[2]  = p->post;
   ID3D11DeviceContext_OMSetRenderTargets(context, 1, &o->rtv, NULL);
   viewport(context, w, h);
   ID3D11DeviceContext_PSSetShader(context, p->composite, NULL, 0);
   ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 3, cbs);
   ID3D11DeviceContext_PSSetShaderResources(context, 0, 7, srvs);
   ID3D11DeviceContext_Draw(context, 3, 0);
   ID3D11DeviceContext_PSSetShaderResources(context, 0, 7, none);
}

bool hdr_d3d11_encode(hdr_d3d11_t *e, ID3D11DeviceContext *context,
      const struct hdr_output *out, ID3D11ShaderResourceView *picture,
      unsigned width, unsigned height, bool unorm8, unsigned index,
      ID3D11Texture2D **texture, ID3D11ShaderResourceView **view)
{
   hdr_d3d11_output_t       *o;
   D3D11_MAPPED_SUBRESOURCE  map;
   struct hdr_dt_block      *block;
   ID3D11ShaderResourceView *srvs[5];
   ID3D11SamplerState       *samplers[2];
   ID3D11Buffer             *cbs[2];
   ID3D11ShaderResourceView *none = NULL;
   D3D11_VIEWPORT            vp;

   if (!e || !picture || width == 0 || height == 0 || index >= HDR_D3D11_MAX_OUTPUTS)
      return false;
   o = &e->outputs[index];
   if (o->texture && (o->width != width || o->height != height))
      output_release(o);
   if (!o->texture && !output_make(e, o, width, height))
      return false;

   if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource*)e->dt, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      return false;
   block = (struct hdr_dt_block*)map.pData;
   hdr_dt_fill(block, out->paper_white_nits, out->max_nits,
         out->panel_peak_nits, out->panel_xy);
   tables_update(e);
   /* the cube only once the texture of it is here, whatever the block says */
   if (!e->tables[T_CUBE])
      block->gm[2] = 0.0f;
   ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)e->dt, 0);
   if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource*)e->params, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      return false;
   hdr_encode_params_fill((struct hdr_encode_params*)map.pData, out, unorm8);
   ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)e->params, 0);

   srvs[0]     = picture;
   srvs[1]     = table_view(e, T_ACES2_LUT);
   srvs[2]     = table_view(e, T_FILM_LUT);
   srvs[3]     = table_view(e, T_DESAT);
   srvs[4]     = table_view(e, T_CUBE);
   samplers[0] = e->linear_clamp;
   samplers[1] = e->linear_wrap;
   cbs[0]      = e->dt;
   cbs[1]      = e->params;
   vp.TopLeftX = 0.0f;
   vp.TopLeftY = 0.0f;
   vp.Width    = (float)width;
   vp.Height   = (float)height;
   vp.MinDepth = 0.0f;
   vp.MaxDepth = 1.0f;

   ID3D11DeviceContext_OMSetRenderTargets(context, 1, &o->rtv, NULL);
   ID3D11DeviceContext_OMSetBlendState(context, NULL, NULL, 0xffffffffu);
   ID3D11DeviceContext_OMSetDepthStencilState(context, NULL, 0);
   ID3D11DeviceContext_RSSetState(context, e->raster);
   ID3D11DeviceContext_RSSetViewports(context, 1, &vp);
   ID3D11DeviceContext_IASetInputLayout(context, NULL);
   ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ID3D11DeviceContext_VSSetShader(context, e->vs, NULL, 0);
   ID3D11DeviceContext_GSSetShader(context, NULL, NULL, 0);
   ID3D11DeviceContext_PSSetShader(context, e->ps, NULL, 0);
   ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 2, cbs);
   ID3D11DeviceContext_PSSetShaderResources(context, 0, 5, srvs);
   ID3D11DeviceContext_PSSetSamplers(context, 0, 2, samplers);
   if (hdr_post_enabled())
   {
      struct hdr_post_plan plan;
      hdr_post_plan_make(&plan, width, height);
      /* (what cannot be made: the picture encoded without them) */
      if (post_ready(e, &plan, width, height))
      {
         post_encode(e, context, &plan, o, width, height);
         ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
         *texture = o->texture;
         *view    = o->view;
         return true;
      }
   }
   ID3D11DeviceContext_Draw(context, 3, 0);
   /* (the picture is a render target again in the next frame) */
   ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &none);
   ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);

   *texture = o->texture;
   *view    = o->view;
   return true;
}
