/* See hdr_encode_vk.h. C89.
 *
 * One full-screen triangle a frame, in a command buffer of its own
 * submitted after the frame's on the queue the frontend and the core
 * share: what wrote the picture - a render pass, a compute dispatch, an
 * upload - is earlier on that queue, and a barrier at the top orders it.
 * Nothing is waited for in a frame but a slot's fence coming round, as
 * the renderers' own command pools do. What goes - an output image of the
 * size before, the tables of the settings before, their staging - goes
 * when the slot that last used it comes round again. */
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_symbol_wrapper.h>
#include <libretro_vulkan.h>

#include "hdr_encode_vk.h"
#include "../hdr/hdr_output.h"
#include "../hdr/hdr_transform.h"

#define HDR_VK_MAX_RINGS   4
#define HDR_VK_MAX_SLOTS   16
#define HDR_VK_MAX_OUTPUTS 32
/* What a frame can retire: the four tables, their staging and one output */
#define HDR_VK_MAX_RETIRED 8

/* Bindings, as hdr.glsl and hdr_transform.glsl have them */
#define B_PICTURE   0
#define B_DT        8
#define B_ACES2_LUT 9
#define B_FILM_LUT  10
#define B_DESAT     11
#define B_CUBE      12

#define HOST_MEMORY (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

typedef struct hdr_vk_image
{
   VkImage          image;
   VkImageView      view;
   VkFramebuffer    framebuffer;   /* the outputs' */
   vk_heap_alloc_t  mem;
   unsigned         width;
   unsigned         height;
   int              live;
} hdr_vk_image_t;

typedef struct hdr_vk_buffer
{
   VkBuffer         buffer;
   vk_heap_alloc_t  mem;
   int              live;
} hdr_vk_buffer_t;

/* What a command pool slot's frame writes, and what it let go of */
typedef struct hdr_vk_slot
{
   uint64_t         epoch;
   int              prepared;
   VkDescriptorSet  set_sampled;
   VkDescriptorSet  set_subpass;
   hdr_vk_buffer_t  dt;            /* the transform's constants */
   hdr_vk_image_t   retired_images[HDR_VK_MAX_RETIRED];
   unsigned         retired_image_count;
   hdr_vk_buffer_t  retired_buffers[HDR_VK_MAX_RETIRED];
   unsigned         retired_buffer_count;
} hdr_vk_slot_t;

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

/* the two kinds of encode: the picture sampled, or an input attachment */
enum
{
   K_SAMPLED = 0,
   K_SUBPASS,
   K_COUNT
};

struct hdr_vk
{
   const struct retro_hw_render_interface_vulkan *iface;
   vk_heap_t            *heap;
   VkDevice              device;
   VkFormat              format;
   VkPipelineCache       cache;
   VkRenderPass          render_pass;   /* the pass of its own */
   VkDescriptorSetLayout set_layout[K_COUNT];
   VkPipelineLayout      pipeline_layout[K_COUNT];
   VkPipeline            pipeline;      /* the pass of its own's */
   VkShaderModule        vert;
   VkShaderModule        frag_subpass;
   VkDescriptorPool      descriptor_pool;
   VkSampler             nearest;
   VkSampler             linear_clamp;
   VkSampler             linear_wrap;
   hdr_vk_slot_t         slots[HDR_VK_MAX_RINGS][HDR_VK_MAX_SLOTS];
   unsigned              slot_count;
   hdr_vk_slot_t        *current;       /* the slot preparing: where what goes is retired */
   hdr_vk_image_t        outputs[HDR_VK_MAX_OUTPUTS];
   hdr_vk_image_t        tables[T_COUNT];
   unsigned              generation;
};

/* ---- memory ---- */

static bool image_bind(hdr_vk_t *e, hdr_vk_image_t *img)
{
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(e->device, img->image, &req);
   if (!vk_heap_alloc(e->heap, &req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &img->mem))
      return false;
   return vkBindImageMemory(e->device, img->image, img->mem.memory, img->mem.offset) == VK_SUCCESS;
}

static void image_destroy(hdr_vk_t *e, hdr_vk_image_t *img)
{
   if (!img->live)
      return;
   if (img->framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(e->device, img->framebuffer, NULL);
   if (img->view != VK_NULL_HANDLE)
      vkDestroyImageView(e->device, img->view, NULL);
   if (img->image != VK_NULL_HANDLE)
      vkDestroyImage(e->device, img->image, NULL);
   if (img->mem.memory != VK_NULL_HANDLE)
      vk_heap_free(e->heap, &img->mem);
   memset(img, 0, sizeof(*img));
}

static bool buffer_make(hdr_vk_t *e, hdr_vk_buffer_t *b, VkDeviceSize size,
      VkBufferUsageFlags usage)
{
   VkBufferCreateInfo   info;
   VkMemoryRequirements req;
   memset(b, 0, sizeof(*b));
   memset(&info, 0, sizeof(info));
   info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   info.size        = size;
   info.usage       = usage;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   if (vkCreateBuffer(e->device, &info, NULL, &b->buffer) != VK_SUCCESS)
      return false;
   b->live = 1;
   vkGetBufferMemoryRequirements(e->device, b->buffer, &req);
   if (     !vk_heap_alloc(e->heap, &req, HOST_MEMORY, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &b->mem)
         || b->mem.mapped == NULL
         || vkBindBufferMemory(e->device, b->buffer, b->mem.memory, b->mem.offset) != VK_SUCCESS)
      return false;
   return true;
}

static void buffer_destroy(hdr_vk_t *e, hdr_vk_buffer_t *b)
{
   if (!b->live)
      return;
   if (b->buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(e->device, b->buffer, NULL);
   if (b->mem.memory != VK_NULL_HANDLE)
      vk_heap_free(e->heap, &b->mem);
   memset(b, 0, sizeof(*b));
}

/* Into the current slot's list, which a frame cannot fill: it is emptied
 * each time the slot comes round, and a frame retires at most
 * HDR_VK_MAX_RETIRED of each. */
static void retire_image(hdr_vk_t *e, hdr_vk_image_t *img)
{
   hdr_vk_slot_t *s = e->current;
   if (!img->live || !s || s->retired_image_count == HDR_VK_MAX_RETIRED)
      return;
   s->retired_images[s->retired_image_count++] = *img;
   memset(img, 0, sizeof(*img));
}

static void retire_buffer(hdr_vk_t *e, hdr_vk_buffer_t *b)
{
   hdr_vk_slot_t *s = e->current;
   if (!b->live || !s || s->retired_buffer_count == HDR_VK_MAX_RETIRED)
      return;
   s->retired_buffers[s->retired_buffer_count++] = *b;
   memset(b, 0, sizeof(*b));
}

static void slot_release(hdr_vk_t *e, hdr_vk_slot_t *s)
{
   unsigned i;
   for (i = 0; i < s->retired_image_count; i++)
      image_destroy(e, &s->retired_images[i]);
   s->retired_image_count = 0;
   for (i = 0; i < s->retired_buffer_count; i++)
      buffer_destroy(e, &s->retired_buffers[i]);
   s->retired_buffer_count = 0;
}

/* ---- the Display Transform's tables ---- */

static void layout_barrier(VkCommandBuffer cmd, VkImage image,
      VkImageLayout from, VkImageLayout to,
      VkAccessFlags src_access, VkAccessFlags dst_access,
      VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
   VkImageMemoryBarrier b;
   memset(&b, 0, sizeof(b));
   b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.srcAccessMask               = src_access;
   b.dstAccessMask               = dst_access;
   b.oldLayout                   = from;
   b.newLayout                   = to;
   b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
   b.image                       = image;
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = 1;
   b.subresourceRange.layerCount = 1;
   vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* A table as an RGBA16F image, half floats being the 16-bit format every
 * device filters: @data unorm16 of @channels a texel (or half RGBA when
 * @half), NULL for a texel of zero; 3D or 2D. Uploaded by @cmd, its staging
 * retired with this slot. */
static bool table_make(hdr_vk_t *e, VkCommandBuffer cmd, hdr_vk_image_t *t,
      const uint16_t *data, unsigned w, unsigned h, unsigned d,
      unsigned channels, bool half, bool three_d)
{
   VkImageCreateInfo     info;
   VkImageViewCreateInfo view;
   VkBufferImageCopy     copy;
   hdr_vk_buffer_t       staging;
   uint16_t             *texels;
   size_t                count = (size_t)w * h * d;
   size_t                i;
   unsigned              c;

   memset(t, 0, sizeof(*t));
   memset(&info, 0, sizeof(info));
   info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   info.imageType     = three_d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
   info.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
   info.extent.width  = w;
   info.extent.height = h;
   info.extent.depth  = d;
   info.mipLevels     = 1;
   info.arrayLayers   = 1;
   info.samples       = VK_SAMPLE_COUNT_1_BIT;
   info.tiling        = VK_IMAGE_TILING_OPTIMAL;
   info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
   info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (vkCreateImage(e->device, &info, NULL, &t->image) != VK_SUCCESS)
      return false;
   t->live = 1;
   if (!image_bind(e, t))
      return false;

   memset(&view, 0, sizeof(view));
   view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view.image                       = t->image;
   view.viewType                    = three_d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
   view.format                      = info.format;
   view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   view.subresourceRange.levelCount = 1;
   view.subresourceRange.layerCount = 1;
   if (vkCreateImageView(e->device, &view, NULL, &t->view) != VK_SUCCESS)
      return false;

   if (!buffer_make(e, &staging, count * 4 * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
   {
      buffer_destroy(e, &staging);
      return false;
   }
   texels = (uint16_t*)staging.mem.mapped;
   if (data == NULL)
      memset(texels, 0, count * 4 * sizeof(uint16_t));
   else if (half)
      memcpy(texels, data, count * 4 * sizeof(uint16_t));
   else
      for (i = 0; i < count; i++)
         for (c = 0; c < 4; c++)
            texels[i * 4 + c] = hdr_dt_unorm16_to_half(data[i * channels + (c < channels ? c : 0)]);

   memset(&copy, 0, sizeof(copy));
   copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy.imageSubresource.layerCount = 1;
   copy.imageExtent                 = info.extent;
   layout_barrier(cmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         0, VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyBufferToImage(cmd, staging.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
   layout_barrier(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   retire_buffer(e, &staging);
   return true;
}

/* The tables again when hdr_transform.c has new ones: a table it does not
 * have is left out, and the shader reads the stand-in. */
static void tables_update(hdr_vk_t *e, VkCommandBuffer cmd)
{
   const uint16_t *cube;
   const uint16_t *aces2;
   const uint16_t *film;
   const uint16_t *desat;
   unsigned        gen = hdr_dt_generation();
   unsigned        i;

   if (!e->tables[T_DUMMY_2D].live)
   {
      if (!table_make(e, cmd, &e->tables[T_DUMMY_2D], NULL, 1, 1, 1, 4, false, false))
         image_destroy(e, &e->tables[T_DUMMY_2D]);
      if (!table_make(e, cmd, &e->tables[T_DUMMY_3D], NULL, 1, 1, 1, 4, false, true))
         image_destroy(e, &e->tables[T_DUMMY_3D]);
   }
   if (gen == e->generation)
      return;
   e->generation = gen;
   for (i = T_ACES2_LUT; i <= T_CUBE; i++)
      retire_image(e, &e->tables[i]);
   cube  = hdr_dt_aces2_cube();
   aces2 = hdr_dt_aces2_lut();
   film  = hdr_dt_film_lut();
   desat = hdr_dt_film_desat();
   if (cube && !table_make(e, cmd, &e->tables[T_CUBE], cube,
            HDR_DT_ACES2_CUBE, HDR_DT_ACES2_CUBE, HDR_DT_ACES2_CUBE, 4, true, true))
      image_destroy(e, &e->tables[T_CUBE]);
   if (aces2 && !table_make(e, cmd, &e->tables[T_ACES2_LUT], aces2,
            HDR_DT_ACES2_LUT_WIDTH, 1, 1, 4, false, false))
      image_destroy(e, &e->tables[T_ACES2_LUT]);
   if (film && !table_make(e, cmd, &e->tables[T_FILM_LUT], film,
            HDR_DT_FILM_LUT_WIDTH, 1, 1, 1, false, false))
      image_destroy(e, &e->tables[T_FILM_LUT]);
   if (desat && !table_make(e, cmd, &e->tables[T_DESAT], desat,
            HDR_DT_DESAT_WIDTH, HDR_DT_DESAT_HEIGHT, 1, 4, false, false))
      image_destroy(e, &e->tables[T_DESAT]);
}

static VkImageView table_view(hdr_vk_t *e, unsigned t)
{
   if (e->tables[t].live)
      return e->tables[t].view;
   return e->tables[t == T_CUBE ? T_DUMMY_3D : T_DUMMY_2D].view;
}

/* ---- what is handed over ---- */

static bool output_make(hdr_vk_t *e, hdr_vk_image_t *o, unsigned w, unsigned h)
{
   VkImageCreateInfo       info;
   VkImageViewCreateInfo   view;
   VkFramebufferCreateInfo fb;

   memset(o, 0, sizeof(*o));
   memset(&info, 0, sizeof(info));
   info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   info.imageType     = VK_IMAGE_TYPE_2D;
   info.format        = e->format;
   info.extent.width  = w;
   info.extent.height = h;
   info.extent.depth  = 1;
   info.mipLevels     = 1;
   info.arrayLayers   = 1;
   info.samples       = VK_SAMPLE_COUNT_1_BIT;
   info.tiling        = VK_IMAGE_TILING_OPTIMAL;
   /* (sampled by the frontend; a transfer source for its screenshots) */
   info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
   info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (vkCreateImage(e->device, &info, NULL, &o->image) != VK_SUCCESS)
      return false;
   o->live   = 1;
   o->width  = w;
   o->height = h;
   if (!image_bind(e, o))
      return false;

   memset(&view, 0, sizeof(view));
   view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view.image                       = o->image;
   view.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
   view.format                      = e->format;
   view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   view.subresourceRange.levelCount = 1;
   view.subresourceRange.layerCount = 1;
   if (vkCreateImageView(e->device, &view, NULL, &o->view) != VK_SUCCESS)
      return false;

   memset(&fb, 0, sizeof(fb));
   fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   fb.renderPass      = e->render_pass;
   fb.attachmentCount = 1;
   fb.pAttachments    = &o->view;
   fb.width           = w;
   fb.height          = h;
   fb.layers          = 1;
   return vkCreateFramebuffer(e->device, &fb, NULL, &o->framebuffer) == VK_SUCCESS;
}

/* ---- set up ---- */

static VkSampler make_sampler(VkDevice device, VkFilter filter, VkSamplerAddressMode mode)
{
   VkSamplerCreateInfo info;
   VkSampler           s = VK_NULL_HANDLE;
   memset(&info, 0, sizeof(info));
   info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
   info.magFilter    = filter;
   info.minFilter    = filter;
   info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
   info.addressModeU = mode;
   info.addressModeV = mode;
   info.addressModeW = mode;
   info.maxLod       = 0.0f;
   info.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
   if (vkCreateSampler(device, &info, NULL, &s) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return s;
}

static bool make_render_pass(hdr_vk_t *e)
{
   VkAttachmentDescription att;
   VkAttachmentReference   ref;
   VkSubpassDescription    sub;
   VkSubpassDependency     dep;
   VkRenderPassCreateInfo  info;

   memset(&att, 0, sizeof(att));
   att.format         = e->format;
   att.samples        = VK_SAMPLE_COUNT_1_BIT;
   att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
   att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
   att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   ref.attachment     = 0;
   ref.layout         = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   memset(&sub, 0, sizeof(sub));
   sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sub.colorAttachmentCount = 1;
   sub.pColorAttachments    = &ref;
   /* what is written is what the frontend samples next */
   memset(&dep, 0, sizeof(dep));
   dep.srcSubpass    = 0;
   dep.dstSubpass    = VK_SUBPASS_EXTERNAL;
   dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dep.dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
   dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
   memset(&info, 0, sizeof(info));
   info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   info.attachmentCount = 1;
   info.pAttachments    = &att;
   info.subpassCount    = 1;
   info.pSubpasses      = &sub;
   info.dependencyCount = 1;
   info.pDependencies   = &dep;
   return vkCreateRenderPass(e->device, &info, NULL, &e->render_pass) == VK_SUCCESS;
}

static bool make_layouts(hdr_vk_t *e)
{
   static const uint32_t        tables[] = { B_ACES2_LUT, B_FILM_LUT, B_DESAT, B_CUBE };
   VkDescriptorSetLayoutBinding b[6];
   VkDescriptorSetLayoutCreateInfo set;
   VkPushConstantRange          push;
   VkPipelineLayoutCreateInfo   layout;
   unsigned                     i, k;

   push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   push.offset     = 0;
   push.size       = sizeof(struct hdr_encode_params);
   for (k = 0; k < K_COUNT; k++)
   {
      memset(b, 0, sizeof(b));
      b[0].binding         = B_PICTURE;
      b[0].descriptorType  = k == K_SUBPASS ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      for (i = 0; i < 4; i++)
      {
         b[1 + i].binding        = tables[i];
         b[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      }
      b[5].binding        = B_DT;
      b[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      for (i = 0; i < 6; i++)
      {
         b[i].descriptorCount = 1;
         b[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      memset(&set, 0, sizeof(set));
      set.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      set.bindingCount = 6;
      set.pBindings    = b;
      if (vkCreateDescriptorSetLayout(e->device, &set, NULL, &e->set_layout[k]) != VK_SUCCESS)
         return false;
      memset(&layout, 0, sizeof(layout));
      layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      layout.setLayoutCount         = 1;
      layout.pSetLayouts            = &e->set_layout[k];
      layout.pushConstantRangeCount = 1;
      layout.pPushConstantRanges    = &push;
      if (vkCreatePipelineLayout(e->device, &layout, NULL, &e->pipeline_layout[k]) != VK_SUCCESS)
         return false;
   }
   return true;
}

static VkShaderModule make_module(VkDevice device, const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo info;
   VkShaderModule           m = VK_NULL_HANDLE;
   memset(&info, 0, sizeof(info));
   info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   info.codeSize = size;
   info.pCode    = code;
   if (vkCreateShaderModule(device, &info, NULL, &m) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return m;
}

/* The triangle over the picture into colour attachment 0 of @subpass */
static VkPipeline make_pipeline(hdr_vk_t *e, VkShaderModule frag, VkPipelineLayout layout,
      VkRenderPass render_pass, uint32_t subpass)
{
   static const VkDynamicState          dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineShaderStageCreateInfo      stages[2];
   VkPipelineVertexInputStateCreateInfo vertex;
   VkPipelineInputAssemblyStateCreateInfo assembly;
   VkPipelineViewportStateCreateInfo    viewport;
   VkPipelineRasterizationStateCreateInfo raster;
   VkPipelineMultisampleStateCreateInfo multisample;
   VkPipelineColorBlendAttachmentState  blend_att;
   VkPipelineColorBlendStateCreateInfo  blend;
   VkPipelineDynamicStateCreateInfo     dynamic;
   VkGraphicsPipelineCreateInfo         info;
   VkPipeline                           pipeline = VK_NULL_HANDLE;

   memset(stages, 0, sizeof(stages));
   stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = e->vert;
   stages[0].pName  = "main";
   stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = frag;
   stages[1].pName  = "main";
   memset(&vertex, 0, sizeof(vertex));
   vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   memset(&assembly, 0, sizeof(assembly));
   assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
   assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   memset(&viewport, 0, sizeof(viewport));
   viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
   viewport.viewportCount = 1;
   viewport.scissorCount  = 1;
   memset(&raster, 0, sizeof(raster));
   raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
   raster.polygonMode = VK_POLYGON_MODE_FILL;
   raster.cullMode    = VK_CULL_MODE_NONE;
   raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   raster.lineWidth   = 1.0f;
   memset(&multisample, 0, sizeof(multisample));
   multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
   multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   memset(&blend_att, 0, sizeof(blend_att));
   blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
   memset(&blend, 0, sizeof(blend));
   blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
   blend.attachmentCount = 1;
   blend.pAttachments    = &blend_att;
   memset(&dynamic, 0, sizeof(dynamic));
   dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
   dynamic.dynamicStateCount = 2;
   dynamic.pDynamicStates    = dynamic_states;
   memset(&info, 0, sizeof(info));
   info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   info.stageCount          = 2;
   info.pStages             = stages;
   info.pVertexInputState   = &vertex;
   info.pInputAssemblyState = &assembly;
   info.pViewportState      = &viewport;
   info.pRasterizationState = &raster;
   info.pMultisampleState   = &multisample;
   info.pColorBlendState    = &blend;
   info.pDynamicState       = &dynamic;
   info.layout              = layout;
   info.renderPass          = render_pass;
   info.subpass             = subpass;
   if (vkCreateGraphicsPipelines(e->device, e->cache, 1, &info, NULL, &pipeline) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return pipeline;
}

static bool make_slots(hdr_vk_t *e)
{
   VkDescriptorPoolSize        sizes[3];
   VkDescriptorPoolCreateInfo  pool;
   VkDescriptorSetAllocateInfo alloc;
   unsigned                    sets = HDR_VK_MAX_RINGS * e->slot_count;
   unsigned                    r, i;

   sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   sizes[0].descriptorCount = sets * 9;
   sizes[1].type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
   sizes[1].descriptorCount = sets;
   sizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   sizes[2].descriptorCount = sets * 2;
   memset(&pool, 0, sizeof(pool));
   pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   pool.maxSets       = sets * 2;
   pool.poolSizeCount = 3;
   pool.pPoolSizes    = sizes;
   if (vkCreateDescriptorPool(e->device, &pool, NULL, &e->descriptor_pool) != VK_SUCCESS)
      return false;
   memset(&alloc, 0, sizeof(alloc));
   alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   alloc.descriptorPool     = e->descriptor_pool;
   alloc.descriptorSetCount = 1;
   for (r = 0; r < HDR_VK_MAX_RINGS; r++)
      for (i = 0; i < e->slot_count; i++)
      {
         hdr_vk_slot_t *s = &e->slots[r][i];
         alloc.pSetLayouts = &e->set_layout[K_SAMPLED];
         if (vkAllocateDescriptorSets(e->device, &alloc, &s->set_sampled) != VK_SUCCESS)
            return false;
         alloc.pSetLayouts = &e->set_layout[K_SUBPASS];
         if (vkAllocateDescriptorSets(e->device, &alloc, &s->set_subpass) != VK_SUCCESS)
            return false;
         if (!buffer_make(e, &s->dt, sizeof(struct hdr_dt_block), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return false;
      }
   return true;
}

hdr_vk_t *hdr_vk_new(const struct retro_hw_render_interface_vulkan *iface,
      vk_heap_t *heap, VkPipelineCache cache, VkFormat format,
      unsigned slots, const hdr_vk_code_t *code)
{
   VkFormatProperties props;
   VkShaderModule     frag;
   hdr_vk_t          *e;

   vkGetPhysicalDeviceFormatProperties(iface->gpu, format, &props);
   if ((props.optimalTilingFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
         != (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      return NULL;
   e = (hdr_vk_t*)calloc(1, sizeof(*e));
   if (!e)
      return NULL;
   e->iface        = iface;
   e->heap         = heap;
   e->device       = iface->device;
   e->format       = format;
   e->cache        = cache;
   e->slot_count   = slots < 1 ? 1 : (slots > HDR_VK_MAX_SLOTS ? HDR_VK_MAX_SLOTS : slots);
   e->generation   = hdr_dt_generation() - 1;
   e->nearest      = make_sampler(e->device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
   e->linear_clamp = make_sampler(e->device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
   e->linear_wrap  = make_sampler(e->device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
   e->vert         = make_module(e->device, code->vert, code->vert_size);
   e->frag_subpass = make_module(e->device, code->frag_subpass, code->frag_subpass_size);
   frag            = make_module(e->device, code->frag, code->frag_size);
   if (     e->nearest != VK_NULL_HANDLE && e->linear_clamp != VK_NULL_HANDLE
         && e->linear_wrap != VK_NULL_HANDLE && e->vert != VK_NULL_HANDLE
         && e->frag_subpass != VK_NULL_HANDLE && frag != VK_NULL_HANDLE
         && make_render_pass(e) && make_layouts(e))
      e->pipeline = make_pipeline(e, frag, e->pipeline_layout[K_SAMPLED], e->render_pass, 0);
   if (frag != VK_NULL_HANDLE)
      vkDestroyShaderModule(e->device, frag, NULL);
   if (e->pipeline == VK_NULL_HANDLE || !make_slots(e))
   {
      hdr_vk_free(e);
      return NULL;
   }
   return e;
}

void hdr_vk_free(hdr_vk_t *e)
{
   unsigned r, i, k;
   if (!e)
      return;
   for (r = 0; r < HDR_VK_MAX_RINGS; r++)
      for (i = 0; i < HDR_VK_MAX_SLOTS; i++)
      {
         slot_release(e, &e->slots[r][i]);
         buffer_destroy(e, &e->slots[r][i].dt);
      }
   for (i = 0; i < HDR_VK_MAX_OUTPUTS; i++)
      image_destroy(e, &e->outputs[i]);
   for (i = 0; i < T_COUNT; i++)
      image_destroy(e, &e->tables[i]);
   if (e->descriptor_pool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(e->device, e->descriptor_pool, NULL);
   if (e->pipeline != VK_NULL_HANDLE)
      vkDestroyPipeline(e->device, e->pipeline, NULL);
   for (k = 0; k < K_COUNT; k++)
   {
      if (e->pipeline_layout[k] != VK_NULL_HANDLE)
         vkDestroyPipelineLayout(e->device, e->pipeline_layout[k], NULL);
      if (e->set_layout[k] != VK_NULL_HANDLE)
         vkDestroyDescriptorSetLayout(e->device, e->set_layout[k], NULL);
   }
   if (e->vert != VK_NULL_HANDLE)
      vkDestroyShaderModule(e->device, e->vert, NULL);
   if (e->frag_subpass != VK_NULL_HANDLE)
      vkDestroyShaderModule(e->device, e->frag_subpass, NULL);
   if (e->render_pass != VK_NULL_HANDLE)
      vkDestroyRenderPass(e->device, e->render_pass, NULL);
   if (e->nearest != VK_NULL_HANDLE)
      vkDestroySampler(e->device, e->nearest, NULL);
   if (e->linear_clamp != VK_NULL_HANDLE)
      vkDestroySampler(e->device, e->linear_clamp, NULL);
   if (e->linear_wrap != VK_NULL_HANDLE)
      vkDestroySampler(e->device, e->linear_wrap, NULL);
   free(e);
}

VkFormat hdr_vk_format(const hdr_vk_t *e)
{
   return e->format;
}

/* ---- a frame ---- */

static hdr_vk_slot_t *slot_of(hdr_vk_t *e, unsigned ring, unsigned slot)
{
   if (ring >= HDR_VK_MAX_RINGS || slot >= e->slot_count)
      return NULL;
   return &e->slots[ring][slot];
}

void hdr_vk_prepare(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      unsigned ring, unsigned slot, uint64_t epoch)
{
   hdr_vk_slot_t       *s = slot_of(e, ring, slot);
   struct hdr_dt_block *block;
   if (!s || (s->prepared && s->epoch == epoch))
      return;
   /* the pool waited for this slot's last frame before it began this one:
    * what that frame let go of goes */
   slot_release(e, s);
   s->epoch    = epoch;
   s->prepared = 1;
   e->current  = s;
   block = (struct hdr_dt_block*)s->dt.mem.mapped;
   hdr_dt_fill(block, out->paper_white_nits, out->max_nits,
         out->panel_peak_nits, out->panel_xy);
   tables_update(e, cmd);
   /* the cube only once the image of it is here, whatever the block says */
   if (!e->tables[T_CUBE].live)
      block->gm[2] = 0.0f;
}

static void write_set(hdr_vk_t *e, hdr_vk_slot_t *s, VkDescriptorSet set,
      VkDescriptorType picture_type, VkImageView picture)
{
   VkDescriptorImageInfo  images[5];
   VkDescriptorBufferInfo dt;
   VkWriteDescriptorSet   w[6];
   static const uint32_t  bindings[5] = { B_PICTURE, B_ACES2_LUT, B_FILM_LUT, B_DESAT, B_CUBE };
   unsigned               i;

   images[0].sampler     = picture_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ? VK_NULL_HANDLE : e->nearest;
   images[0].imageView   = picture;
   images[1].sampler     = e->linear_wrap;
   images[1].imageView   = table_view(e, T_ACES2_LUT);
   images[2].sampler     = e->linear_clamp;
   images[2].imageView   = table_view(e, T_FILM_LUT);
   images[3].sampler     = e->linear_clamp;
   images[3].imageView   = table_view(e, T_DESAT);
   images[4].sampler     = e->linear_clamp;
   images[4].imageView   = table_view(e, T_CUBE);
   dt.buffer             = s->dt.buffer;
   dt.offset             = 0;
   dt.range              = sizeof(struct hdr_dt_block);
   memset(w, 0, sizeof(w));
   for (i = 0; i < 5; i++)
   {
      images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      w[i].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet           = set;
      w[i].dstBinding       = bindings[i];
      w[i].descriptorCount  = 1;
      w[i].descriptorType   = i == 0 ? picture_type : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[i].pImageInfo       = &images[i];
   }
   w[5].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   w[5].dstSet          = set;
   w[5].dstBinding      = B_DT;
   w[5].descriptorCount = 1;
   w[5].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   w[5].pBufferInfo     = &dt;
   vkUpdateDescriptorSets(e->device, 6, w, 0, NULL);
}

/* The triangle, with what it reads bound: in a subpass or a pass of its own */
static void draw(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
      unsigned width, unsigned height, bool unorm8)
{
   struct hdr_encode_params params;
   VkViewport               vp;
   VkRect2D                 scissor;
   vp.x                  = 0.0f;
   vp.y                  = 0.0f;
   vp.width              = (float)width;
   vp.height             = (float)height;
   vp.minDepth           = 0.0f;
   vp.maxDepth           = 1.0f;
   scissor.offset.x      = 0;
   scissor.offset.y      = 0;
   scissor.extent.width  = width;
   scissor.extent.height = height;
   vkCmdSetViewport(cmd, 0, 1, &vp);
   vkCmdSetScissor(cmd, 0, 1, &scissor);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
   hdr_encode_params_fill(&params, out, unorm8);
   vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
   vkCmdDraw(cmd, 3, 1, 0, 0);
}

VkPipeline hdr_vk_subpass_pipeline(hdr_vk_t *e, VkRenderPass render_pass, uint32_t subpass)
{
   return make_pipeline(e, e->frag_subpass, e->pipeline_layout[K_SUBPASS], render_pass, subpass);
}

void hdr_vk_subpass_draw(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      unsigned ring, unsigned slot, VkPipeline pipeline, VkImageView view,
      unsigned width, unsigned height, bool unorm8)
{
   hdr_vk_slot_t *s = slot_of(e, ring, slot);
   if (!s || pipeline == VK_NULL_HANDLE)
      return;
   write_set(e, s, s->set_subpass, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, view);
   draw(e, out, cmd, pipeline, e->pipeline_layout[K_SUBPASS], s->set_subpass, width, height, unorm8);
}

bool hdr_vk_encode(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      unsigned ring, unsigned slot, uint64_t epoch, unsigned index,
      VkImage *image, VkImageView *view, unsigned width, unsigned height, bool unorm8)
{
   hdr_vk_slot_t        *s = slot_of(e, ring, slot);
   hdr_vk_image_t       *o;
   VkMemoryBarrier       barrier;
   VkRenderPassBeginInfo rp;

   if (!s || width == 0 || height == 0 || index >= HDR_VK_MAX_OUTPUTS)
      return false;
   hdr_vk_prepare(e, out, cmd, ring, slot, epoch);
   e->current = s;
   o = &e->outputs[index];
   if (o->live && (o->width != width || o->height != height))
      retire_image(e, o);
   if (!o->live && !output_make(e, o, width, height))
   {
      image_destroy(e, o);
      return false;
   }
   write_set(e, s, s->set_sampled, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, *view);

   /* what drew the picture, earlier in the frame, before it is read; what
    * read the output last, before it is written again */
   memset(&barrier, 0, sizeof(barrier));
   barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
   barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT
                         | VK_ACCESS_TRANSFER_WRITE_BIT;
   barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
         | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
         | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         0, 1, &barrier, 0, NULL, 0, NULL);
   memset(&rp, 0, sizeof(rp));
   rp.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rp.renderPass               = e->render_pass;
   rp.framebuffer              = o->framebuffer;
   rp.renderArea.extent.width  = width;
   rp.renderArea.extent.height = height;
   vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
   draw(e, out, cmd, e->pipeline, e->pipeline_layout[K_SAMPLED], s->set_sampled, width, height, unorm8);
   vkCmdEndRenderPass(cmd);

   *image = o->image;
   *view  = o->view;
   return true;
}
