/* The HDR output's encode in HLSL (rend/hdr/hdr_encode.hlsl, Direct3D 11
 * and 12) against the GLSL the Vulkan renderer draws with
 * (rend/vulkan/shaders/hdr_encode.frag): both run on one Vulkan device,
 * the HLSL through glslang's HLSL front end, over the same pictures with
 * the same constants and tables, every Display Transform and every
 * setting the encode reads, into a 32-bit float target; any channel that
 * differs by more than float rounding fails the run. hdr_hlsl.sh.
 *
 *   hdr_hlsl hlsl_ps.spv
 *
 * Needs a Vulkan driver (lavapipe will do); the validation layer is
 * enabled when installed and any error it reports fails the run. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>

#include "rend/hdr/hdr_output.h"
#include "rend/hdr/hdr_transform.h"
#include "rend/hdr/hdr_display.h"
#include "../tools/hdr/hdr_cases.h"

typedef uint32_t u32;
#define RASTER_SPV_WORDS
#include "rend/vulkan/raster_spv.inc"

#define W HDR_CASES_W
#define H HDR_CASES_H

/* --against: what Direct3D drew of each case (tools/hdr/hdr_d3d.c) */
static FILE *against;
static float *against_out;

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static int validation_errors;

#define FN(name) static PFN_##name p_##name
FN(vkCreateImage); FN(vkCreateImageView); FN(vkGetImageMemoryRequirements); FN(vkAllocateMemory);
FN(vkBindImageMemory); FN(vkCreateBuffer); FN(vkGetBufferMemoryRequirements); FN(vkBindBufferMemory);
FN(vkMapMemory); FN(vkCreateCommandPool); FN(vkAllocateCommandBuffers); FN(vkBeginCommandBuffer);
FN(vkEndCommandBuffer); FN(vkCmdPipelineBarrier); FN(vkCmdCopyBufferToImage); FN(vkCmdCopyImageToBuffer);
FN(vkQueueSubmit); FN(vkQueueWaitIdle); FN(vkCreateShaderModule); FN(vkCreateDescriptorSetLayout);
FN(vkCreatePipelineLayout); FN(vkCreateRenderPass); FN(vkCreateFramebuffer); FN(vkCreateGraphicsPipelines);
FN(vkCreateDescriptorPool); FN(vkAllocateDescriptorSets); FN(vkUpdateDescriptorSets); FN(vkCreateSampler);
FN(vkCmdBeginRenderPass); FN(vkCmdEndRenderPass); FN(vkCmdBindPipeline); FN(vkCmdBindDescriptorSets);
FN(vkCmdPushConstants); FN(vkCmdDraw); FN(vkCmdSetViewport); FN(vkCmdSetScissor); FN(vkResetCommandBuffer);
FN(vkGetPhysicalDeviceMemoryProperties);
#define LOADD(name) p_##name = (PFN_##name)gdpa(dev, #name)

static VkPhysicalDeviceMemoryProperties memprops;
static VkCommandBuffer cmd;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   uint32_t i;
   for (i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "hdr_hlsl: no memory type\n");
   exit(2);
}

typedef struct buf { VkBuffer b; void *map; } buf_t;

static buf_t buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
   VkBufferCreateInfo bci;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo mai;
   VkDeviceMemory mem;
   buf_t r;
   memset(&bci, 0, sizeof(bci));
   bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bci.size  = size;
   bci.usage = usage;
   p_vkCreateBuffer(dev, &bci, NULL, &r.b);
   p_vkGetBufferMemoryRequirements(dev, r.b, &req);
   memset(&mai, 0, sizeof(mai));
   mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize  = req.size;
   mai.memoryTypeIndex = memtype(req.memoryTypeBits,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   p_vkAllocateMemory(dev, &mai, NULL, &mem);
   p_vkBindBufferMemory(dev, r.b, mem, 0);
   p_vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &r.map);
   return r;
}

typedef struct img { VkImage i; VkImageView v; } img_t;

static img_t image(VkImageType type, VkFormat format, uint32_t w, uint32_t h, uint32_t d, VkImageUsageFlags usage)
{
   VkImageCreateInfo ici;
   VkImageViewCreateInfo vci;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo mai;
   VkDeviceMemory mem;
   img_t r;
   memset(&ici, 0, sizeof(ici));
   ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   ici.imageType     = type;
   ici.format        = format;
   ici.extent.width  = w;
   ici.extent.height = h;
   ici.extent.depth  = d;
   ici.mipLevels     = 1;
   ici.arrayLayers   = 1;
   ici.samples       = VK_SAMPLE_COUNT_1_BIT;
   ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
   ici.usage         = usage;
   p_vkCreateImage(dev, &ici, NULL, &r.i);
   p_vkGetImageMemoryRequirements(dev, r.i, &req);
   memset(&mai, 0, sizeof(mai));
   mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize  = req.size;
   mai.memoryTypeIndex = memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   p_vkAllocateMemory(dev, &mai, NULL, &mem);
   p_vkBindImageMemory(dev, r.i, mem, 0);
   memset(&vci, 0, sizeof(vci));
   vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   vci.image                       = r.i;
   vci.viewType                    = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
   vci.format                      = format;
   vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   vci.subresourceRange.levelCount = 1;
   vci.subresourceRange.layerCount = 1;
   p_vkCreateImageView(dev, &vci, NULL, &r.v);
   return r;
}

static void begin(void)
{
   VkCommandBufferBeginInfo b;
   memset(&b, 0, sizeof(b));
   b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   p_vkResetCommandBuffer(cmd, 0);
   p_vkBeginCommandBuffer(cmd, &b);
}

static void submit(void)
{
   VkSubmitInfo s;
   p_vkEndCommandBuffer(cmd);
   memset(&s, 0, sizeof(s));
   s.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   s.commandBufferCount = 1;
   s.pCommandBuffers    = &cmd;
   p_vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE);
   p_vkQueueWaitIdle(queue);
}

static void layout(VkImage i, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b;
   memset(&b, 0, sizeof(b));
   b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.srcAccessMask               = VK_ACCESS_MEMORY_WRITE_BIT;
   b.dstAccessMask               = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
   b.oldLayout                   = from;
   b.newLayout                   = to;
   b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
   b.image                       = i;
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = 1;
   b.subresourceRange.layerCount = 1;
   p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         0, 0, NULL, 0, NULL, 1, &b);
}

/* an image filled from @data, @bytes a texel, left shader-readable */
static img_t uploaded(VkImageType type, VkFormat format, uint32_t w, uint32_t h, uint32_t d,
      const void *data, size_t bytes)
{
   img_t r = image(type, format, w, h, d, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
   buf_t s = buffer((VkDeviceSize)w * h * d * bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   VkBufferImageCopy c;
   memcpy(s.map, data, (size_t)w * h * d * bytes);
   memset(&c, 0, sizeof(c));
   c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   c.imageSubresource.layerCount = 1;
   c.imageExtent.width  = w;
   c.imageExtent.height = h;
   c.imageExtent.depth  = d;
   begin();
   layout(r.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   p_vkCmdCopyBufferToImage(cmd, s.b, r.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
   layout(r.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   submit();
   return r;
}

/* a Display Transform table as the encoders make it: RGBA16F */
static img_t table(const uint16_t *data, uint32_t w, uint32_t h, uint32_t d, unsigned channels, int half, int three_d)
{
   size_t n = (size_t)w * h * d, i;
   unsigned c;
   uint16_t *t = (uint16_t*)calloc(n * 4, sizeof(uint16_t));
   img_t r;
   if (data)
   {
      if (half)
         memcpy(t, data, n * 4 * sizeof(uint16_t));
      else
         for (i = 0; i < n; i++)
            for (c = 0; c < 4; c++)
               t[i * 4 + c] = hdr_dt_unorm16_to_half(data[i * channels + (c < channels ? c : 0)]);
   }
   r = uploaded(three_d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, d, t, 8);
   free(t);
   return r;
}

static VkShaderModule module(const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo i;
   VkShaderModule m;
   memset(&i, 0, sizeof(i));
   i.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   i.codeSize = size;
   i.pCode    = code;
   if (p_vkCreateShaderModule(dev, &i, NULL, &m) != VK_SUCCESS)
   {
      fprintf(stderr, "hdr_hlsl: a module could not be made\n");
      exit(2);
   }
   return m;
}

/* ---- the two encodes ---- */

enum { GLSL = 0, HLSL = 1 };

typedef struct encode
{
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout      layout;
   VkPipeline            pipeline;
   VkDescriptorSet       set;
} encode_t;

static VkRenderPass render_pass;
static VkFramebuffer framebuffer;
static img_t target;
static buf_t target_read, dt_ubo, params_ubo;
static img_t picture, tables[4], dummy2, dummy3;
static VkSampler s_nearest, s_clamp, s_wrap;
static VkDescriptorPool pool;
static encode_t enc[2];
static VkShaderModule vert;

static VkSampler sampler(VkFilter f, VkSamplerAddressMode m)
{
   VkSamplerCreateInfo i;
   VkSampler s;
   memset(&i, 0, sizeof(i));
   i.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
   i.magFilter = i.minFilter = f;
   i.addressModeU = i.addressModeV = i.addressModeW = m;
   p_vkCreateSampler(dev, &i, NULL, &s);
   return s;
}

static void make_encode(int kind, VkShaderModule frag)
{
   VkDescriptorSetLayoutBinding b[12];
   VkDescriptorSetLayoutCreateInfo sl;
   VkPushConstantRange push;
   VkPipelineLayoutCreateInfo pl;
   VkDescriptorSetAllocateInfo da;
   VkPipelineShaderStageCreateInfo st[2];
   VkPipelineVertexInputStateCreateInfo vi;
   VkPipelineInputAssemblyStateCreateInfo ia;
   VkPipelineViewportStateCreateInfo vp;
   VkPipelineRasterizationStateCreateInfo rs;
   VkPipelineMultisampleStateCreateInfo ms;
   VkPipelineColorBlendAttachmentState ba;
   VkPipelineColorBlendStateCreateInfo bs;
   VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo ds;
   VkGraphicsPipelineCreateInfo gp;
   unsigned n = 0, i;
   encode_t *e = &enc[kind];

   memset(b, 0, sizeof(b));
   if (kind == GLSL)
   {
      static const uint32_t img_bindings[5] = { 0, 9, 10, 11, 12 };
      for (i = 0; i < 5; i++, n++)
      {
         b[n].binding = img_bindings[i];
         b[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      }
      b[n].binding = 8;
      b[n++].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   }
   else
   {
      for (i = 0; i < 5; i++, n++)
      {
         b[n].binding = i;
         b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      }
      b[n].binding = 10;
      b[n++].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      b[n].binding = 11;
      b[n++].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      b[n].binding = 20;
      b[n++].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      b[n].binding = 21;
      b[n++].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   }
   for (i = 0; i < n; i++)
   {
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   }
   memset(&sl, 0, sizeof(sl));
   sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   sl.bindingCount = n;
   sl.pBindings = b;
   p_vkCreateDescriptorSetLayout(dev, &sl, NULL, &e->set_layout);
   push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
   push.offset = 0;
   push.size = sizeof(struct hdr_encode_params);
   memset(&pl, 0, sizeof(pl));
   pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
   pl.setLayoutCount = 1;
   pl.pSetLayouts = &e->set_layout;
   pl.pushConstantRangeCount = kind == GLSL;
   pl.pPushConstantRanges = &push;
   p_vkCreatePipelineLayout(dev, &pl, NULL, &e->layout);
   memset(&da, 0, sizeof(da));
   da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   da.descriptorPool = pool;
   da.descriptorSetCount = 1;
   da.pSetLayouts = &e->set_layout;
   p_vkAllocateDescriptorSets(dev, &da, &e->set);

   memset(st, 0, sizeof(st));
   st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   st[0].module = vert;
   st[0].pName = "main";
   st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   st[1].module = frag;
   st[1].pName = "main";
   memset(&vi, 0, sizeof(vi));
   vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   memset(&ia, 0, sizeof(ia));
   ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   memset(&vp, 0, sizeof(vp));
   vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
   vp.viewportCount = vp.scissorCount = 1;
   memset(&rs, 0, sizeof(rs));
   rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
   rs.lineWidth = 1.0f;
   memset(&ms, 0, sizeof(ms));
   ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
   ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   memset(&ba, 0, sizeof(ba));
   ba.colorWriteMask = 0xf;
   memset(&bs, 0, sizeof(bs));
   bs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
   bs.attachmentCount = 1;
   bs.pAttachments = &ba;
   memset(&ds, 0, sizeof(ds));
   ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
   ds.dynamicStateCount = 2;
   ds.pDynamicStates = dyn;
   memset(&gp, 0, sizeof(gp));
   gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   gp.stageCount = 2;
   gp.pStages = st;
   gp.pVertexInputState = &vi;
   gp.pInputAssemblyState = &ia;
   gp.pViewportState = &vp;
   gp.pRasterizationState = &rs;
   gp.pMultisampleState = &ms;
   gp.pColorBlendState = &bs;
   gp.pDynamicState = &ds;
   gp.layout = e->layout;
   gp.renderPass = render_pass;
   if (p_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &e->pipeline) != VK_SUCCESS)
   {
      fprintf(stderr, "hdr_hlsl: a pipeline could not be made\n");
      exit(2);
   }
}

static VkImageView table_view(int t)
{
   if (tables[t].v != VK_NULL_HANDLE)
      return tables[t].v;
   return t == 3 ? dummy3.v : dummy2.v;
}

static void write_sets(void)
{
   VkDescriptorImageInfo ii[7];
   VkDescriptorBufferInfo bi[2];
   VkWriteDescriptorSet w[16];
   unsigned n = 0, i;
   static const uint32_t gb[5] = { 0, 9, 10, 11, 12 };
   VkSampler gs[5];
   VkImageView views[5];
   views[0] = picture.v;
   for (i = 0; i < 4; i++)
      views[i + 1] = table_view(i);
   gs[0] = s_nearest; gs[1] = s_wrap; gs[2] = s_clamp; gs[3] = s_clamp; gs[4] = s_clamp;
   memset(w, 0, sizeof(w));
   memset(ii, 0, sizeof(ii));
   for (i = 0; i < 5; i++)
   {
      ii[i].sampler = gs[i];
      ii[i].imageView = views[i];
      ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[n].dstSet = enc[GLSL].set;
      w[n].dstBinding = gb[i];
      w[n].descriptorCount = 1;
      w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[n++].pImageInfo = &ii[i];
      w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[n].dstSet = enc[HLSL].set;
      w[n].dstBinding = i;
      w[n].descriptorCount = 1;
      w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w[n++].pImageInfo = &ii[i];
   }
   ii[5].sampler = s_clamp;
   ii[6].sampler = s_wrap;
   for (i = 0; i < 2; i++)
   {
      w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[n].dstSet = enc[HLSL].set;
      w[n].dstBinding = 10 + i;
      w[n].descriptorCount = 1;
      w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      w[n++].pImageInfo = &ii[5 + i];
   }
   bi[0].buffer = dt_ubo.b;
   bi[0].offset = 0;
   bi[0].range = sizeof(struct hdr_dt_block);
   bi[1].buffer = params_ubo.b;
   bi[1].offset = 0;
   bi[1].range = sizeof(struct hdr_encode_params);
   w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   w[n].dstSet = enc[GLSL].set;
   w[n].dstBinding = 8;
   w[n].descriptorCount = 1;
   w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   w[n++].pBufferInfo = &bi[0];
   for (i = 0; i < 2; i++)
   {
      w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[n].dstSet = enc[HLSL].set;
      w[n].dstBinding = 20 + i;
      w[n].descriptorCount = 1;
      w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[n++].pBufferInfo = &bi[i];
   }
   p_vkUpdateDescriptorSets(dev, n, w, 0, NULL);
}

static void draw(int kind, float *out)
{
   VkRenderPassBeginInfo rp;
   VkViewport v;
   VkRect2D sc;
   VkBufferImageCopy c;
   encode_t *e = &enc[kind];
   memset(&rp, 0, sizeof(rp));
   rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rp.renderPass = render_pass;
   rp.framebuffer = framebuffer;
   rp.renderArea.extent.width = W;
   rp.renderArea.extent.height = H;
   begin();
   p_vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
   v.x = v.y = 0.0f;
   v.width = W;
   v.height = H;
   v.minDepth = 0.0f;
   v.maxDepth = 1.0f;
   sc = rp.renderArea;
   p_vkCmdSetViewport(cmd, 0, 1, &v);
   p_vkCmdSetScissor(cmd, 0, 1, &sc);
   p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->pipeline);
   p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->layout, 0, 1, &e->set, 0, NULL);
   if (kind == GLSL)
      p_vkCmdPushConstants(cmd, e->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(struct hdr_encode_params), params_ubo.map);
   p_vkCmdDraw(cmd, 3, 1, 0, 0);
   p_vkCmdEndRenderPass(cmd);
   memset(&c, 0, sizeof(c));
   c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   c.imageSubresource.layerCount = 1;
   c.imageExtent.width = W;
   c.imageExtent.height = H;
   c.imageExtent.depth = 1;
   p_vkCmdCopyImageToBuffer(cmd, target.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target_read.b, 1, &c);
   submit();
   memcpy(out, target_read.map, (size_t)W * H * 4 * sizeof(float));
}

static void make_tables(void)
{
   const uint16_t *cube = hdr_dt_aces2_cube(), *aces2 = hdr_dt_aces2_lut();
   const uint16_t *film = hdr_dt_film_lut(), *desat = hdr_dt_film_desat();
   memset(tables, 0, sizeof(tables));
   if (aces2)
      tables[0] = table(aces2, HDR_DT_ACES2_LUT_WIDTH, 1, 1, 4, 0, 0);
   if (film)
      tables[1] = table(film, HDR_DT_FILM_LUT_WIDTH, 1, 1, 1, 0, 0);
   if (desat)
      tables[2] = table(desat, HDR_DT_DESAT_WIDTH, HDR_DT_DESAT_HEIGHT, 1, 4, 0, 0);
   if (cube)
      tables[3] = table(cube, HDR_DT_ACES2_CUBE, HDR_DT_ACES2_CUBE, HDR_DT_ACES2_CUBE, 4, 1, 1);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
      VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u)
{
   (void)type; (void)u;
   if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
   {
      validation_errors++;
      fprintf(stderr, "hdr_hlsl: validation: %s\n", d->pMessage);
   }
   return VK_FALSE;
}

static void *load_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   void *buf;
   long n;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = malloc((size_t)n);
   if (fread(buf, 1, (size_t)n, f) != (size_t)n)
      n = 0;
   fclose(f);
   *size = (size_t)n;
   return buf;
}

static void setup(const char *hlsl_path)
{
   void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   PFN_vkCreateInstance p_vkCreateInstance;
   PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceQueueFamilyProperties p_qf;
   PFN_vkCreateDevice p_vkCreateDevice;
   PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
   PFN_vkGetDeviceProcAddr gdpa;
   PFN_vkEnumerateInstanceLayerProperties p_layers;
   VkLayerProperties layers[64];
   uint32_t nl = 64, n = 1, i;
   const char *layer = "VK_LAYER_KHRONOS_validation", *ext = "VK_EXT_debug_utils";
   int validation = 0;
   VkApplicationInfo app;
   VkInstanceCreateInfo ici;
   VkDebugUtilsMessengerCreateInfoEXT dci;
   VkDebugUtilsMessengerEXT messenger;
   VkQueueFamilyProperties qf[16];
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci;
   VkDeviceCreateInfo dci2;
   VkCommandPoolCreateInfo cpi;
   VkCommandBufferAllocateInfo cbi;
   VkAttachmentDescription att;
   VkAttachmentReference ref;
   VkSubpassDescription sub;
   VkRenderPassCreateInfo rpi;
   VkFramebufferCreateInfo fbi;
   VkDescriptorPoolSize ps[4];
   VkDescriptorPoolCreateInfo dpi;
   size_t hsize;
   void *hcode;
   static const uint16_t zero[4] = { 0, 0, 0, 0 };

   if (!vk)
   {
      fprintf(stderr, "hdr_hlsl: no Vulkan loader\n");
      exit(2);
   }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
   p_vkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   p_layers = (PFN_vkEnumerateInstanceLayerProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
   if (p_layers(&nl, layers) >= 0)
      for (i = 0; i < nl; i++)
         if (!strcmp(layers[i].layerName, layer))
            validation = 1;
   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.apiVersion = VK_API_VERSION_1_0;
   memset(&dci, 0, sizeof(dci));
   dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
   dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
   dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
   dci.pfnUserCallback = debug_cb;
   memset(&ici, 0, sizeof(ici));
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &app;
   if (validation)
   {
      ici.pNext = &dci;
      ici.enabledLayerCount = 1;
      ici.ppEnabledLayerNames = &layer;
      ici.enabledExtensionCount = 1;
      ici.ppEnabledExtensionNames = &ext;
   }
   if (p_vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
      exit(2);
   if (validation)
      ((PFN_vkCreateDebugUtilsMessengerEXT)gipa(instance, "vkCreateDebugUtilsMessengerEXT"))(instance, &dci, NULL, &messenger);
   p_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
   p_vkEnumeratePhysicalDevices(instance, &n, &gpu);
   p_qf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
   n = 16;
   p_qf(gpu, &n, qf);
   for (family = 0; family < n && !(qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++) ;
   memset(&qci, 0, sizeof(qci));
   qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   qci.queueFamilyIndex = family;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   memset(&dci2, 0, sizeof(dci2));
   dci2.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   dci2.queueCreateInfoCount = 1;
   dci2.pQueueCreateInfos = &qci;
   p_vkCreateDevice = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   if (p_vkCreateDevice(gpu, &dci2, NULL, &dev) != VK_SUCCESS)
      exit(2);
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   p_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue");
   p_vkGetDeviceQueue(dev, family, 0, &queue);
   p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
      gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
   p_vkGetPhysicalDeviceMemoryProperties(gpu, &memprops);
   LOADD(vkCreateImage); LOADD(vkCreateImageView); LOADD(vkGetImageMemoryRequirements); LOADD(vkAllocateMemory);
   LOADD(vkBindImageMemory); LOADD(vkCreateBuffer); LOADD(vkGetBufferMemoryRequirements); LOADD(vkBindBufferMemory);
   LOADD(vkMapMemory); LOADD(vkCreateCommandPool); LOADD(vkAllocateCommandBuffers); LOADD(vkBeginCommandBuffer);
   LOADD(vkEndCommandBuffer); LOADD(vkCmdPipelineBarrier); LOADD(vkCmdCopyBufferToImage); LOADD(vkCmdCopyImageToBuffer);
   LOADD(vkQueueSubmit); LOADD(vkQueueWaitIdle); LOADD(vkCreateShaderModule); LOADD(vkCreateDescriptorSetLayout);
   LOADD(vkCreatePipelineLayout); LOADD(vkCreateRenderPass); LOADD(vkCreateFramebuffer); LOADD(vkCreateGraphicsPipelines);
   LOADD(vkCreateDescriptorPool); LOADD(vkAllocateDescriptorSets); LOADD(vkUpdateDescriptorSets); LOADD(vkCreateSampler);
   LOADD(vkCmdBeginRenderPass); LOADD(vkCmdEndRenderPass); LOADD(vkCmdBindPipeline); LOADD(vkCmdBindDescriptorSets);
   LOADD(vkCmdPushConstants); LOADD(vkCmdDraw); LOADD(vkCmdSetViewport); LOADD(vkCmdSetScissor); LOADD(vkResetCommandBuffer);

   memset(&cpi, 0, sizeof(cpi));
   cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cpi.queueFamilyIndex = family;
   {
      VkCommandPool cp;
      p_vkCreateCommandPool(dev, &cpi, NULL, &cp);
      memset(&cbi, 0, sizeof(cbi));
      cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cbi.commandPool = cp;
      cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cbi.commandBufferCount = 1;
      p_vkAllocateCommandBuffers(dev, &cbi, &cmd);
   }

   /* a 32-bit float target: what each shader returns, not rounded */
   memset(&att, 0, sizeof(att));
   att.format = VK_FORMAT_R32G32B32A32_SFLOAT;
   att.samples = VK_SAMPLE_COUNT_1_BIT;
   att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   ref.attachment = 0;
   ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   memset(&sub, 0, sizeof(sub));
   sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sub.colorAttachmentCount = 1;
   sub.pColorAttachments = &ref;
   memset(&rpi, 0, sizeof(rpi));
   rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   rpi.attachmentCount = 1;
   rpi.pAttachments = &att;
   rpi.subpassCount = 1;
   rpi.pSubpasses = &sub;
   p_vkCreateRenderPass(dev, &rpi, NULL, &render_pass);
   target = image(VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, W, H, 1,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   memset(&fbi, 0, sizeof(fbi));
   fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   fbi.renderPass = render_pass;
   fbi.attachmentCount = 1;
   fbi.pAttachments = &target.v;
   fbi.width = W;
   fbi.height = H;
   fbi.layers = 1;
   p_vkCreateFramebuffer(dev, &fbi, NULL, &framebuffer);
   target_read = buffer((VkDeviceSize)W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   dt_ubo = buffer(sizeof(struct hdr_dt_block), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   params_ubo = buffer(sizeof(struct hdr_encode_params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   s_nearest = sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
   s_clamp = sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
   s_wrap = sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
   dummy2 = uploaded(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1, zero, 8);
   dummy3 = uploaded(VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1, zero, 8);

   /* the picture (hdr_cases.h) */
   {
      unsigned char *px = (unsigned char*)malloc((size_t)W * H * 4);
      hdr_cases_picture(px);
      picture = uploaded(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, W, H, 1, px, 4);
      free(px);
   }

   ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = 5;
   ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; ps[1].descriptorCount = 5;
   ps[2].type = VK_DESCRIPTOR_TYPE_SAMPLER; ps[2].descriptorCount = 2;
   ps[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[3].descriptorCount = 3;
   memset(&dpi, 0, sizeof(dpi));
   dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpi.maxSets = 2;
   dpi.poolSizeCount = 4;
   dpi.pPoolSizes = ps;
   p_vkCreateDescriptorPool(dev, &dpi, NULL, &pool);

   vert = module(raster_spv[RSPV_SSAA_RESOLVE_VERT], raster_spv_size[RSPV_SSAA_RESOLVE_VERT]);
   make_encode(GLSL, module(raster_spv[RSPV_HDR_ENCODE_FRAG], raster_spv_size[RSPV_HDR_ENCODE_FRAG]));
   hcode = load_file(hlsl_path, &hsize);
   if (!hcode || !hsize)
   {
      fprintf(stderr, "hdr_hlsl: cannot read %s\n", hlsl_path);
      exit(2);
   }
   make_encode(HLSL, module((const uint32_t*)hcode, hsize));
}

/* ---- the cases ---- */

static float glsl_out[W * H * 4], hlsl_out[W * H * 4];
static unsigned cases, failures;

/* What Direct3D drew of this case, in the format handed over, against the
 * GLSL: the same float roundings apart, and the output's own rounding -
 * half a 10-bit code for HDR10, a half float's for scRGB */
static void against_case(const char *name, const struct hdr_output *o)
{
   const int scrgb = o->format == HDR_OUTPUT_SCRGB;
   unsigned i, at = 0;
   double worst = 0.0;
   if (fread(against_out, sizeof(float), (size_t)W * H * 4, against) != (size_t)W * H * 4)
   {
      failures++;
      printf("hdr_hlsl: %s: no Direct3D picture\n", name);
      return;
   }
   for (i = 0; i < W * H * 4; i++)
   {
      double a = glsl_out[i], d = against_out[i];
      double tol = scrgb ? 5e-4 * (fabs(a) > 1.0 ? fabs(a) : 1.0) + fabs(a) / 1024.0 + 1e-7
            : (i % 4 == 3 ? 1e-6 : 0.5 / 1023.0 + 5e-4);
      double over = fabs(a - d) / tol;
      if (!(over <= worst))
      {
         worst = over;
         at = i;
      }
   }
   if (!(worst <= 1.0))
   {
      failures++;
      printf("hdr_hlsl: %s: Direct3D pixel %u,%u channel %u: GLSL %g, Direct3D %g\n", name,
            (at / 4) % W, (at / 4) / W, at % 4, glsl_out[at], against_out[at]);
   }
   else if (getenv("HDR_HLSL_VERBOSE"))
      printf("hdr_hlsl: %s: Direct3D within %.2f of the tolerance\n", name, worst);
}

static void run_case(const char *name, const struct hdr_dt_settings *dt, const struct hdr_output *o,
      int deband, int cube)
{
   struct hdr_dt_block *b = (struct hdr_dt_block*)dt_ubo.map;
   unsigned i;
   double worst = 0.0;
   unsigned at = 0;
   hdr_dt_configure(dt);
   if (cube)
   {
      /* (the tables made for these settings first, the cube from them) */
      hdr_dt_fill(b, o->paper_white_nits, o->max_nits, o->panel_peak_nits, o->panel_xy);
      hdr_dt_aces2_cube_finish(o->paper_white_nits);
   }
   hdr_dt_fill(b, o->paper_white_nits, o->max_nits, o->panel_peak_nits, o->panel_xy);
   make_tables();
   if (cube && (tables[3].v == VK_NULL_HANDLE || b->gm[2] == 0.0f))
   {
      failures++;
      printf("hdr_hlsl: %s: drawn without its cube\n", name);
   }
   if (!cube || tables[3].v == VK_NULL_HANDLE)
      b->gm[2] = 0.0f;
   hdr_encode_params_fill((struct hdr_encode_params*)params_ubo.map, o, deband);
   write_sets();
   draw(GLSL, glsl_out);
   draw(HLSL, hlsl_out);
   for (i = 0; i < W * H * 4; i++)
   {
      double a = glsl_out[i], h = hlsl_out[i];
      double d = fabs(a - h) / (fabs(a) > 1.0 ? fabs(a) : 1.0);
      if (!(d <= worst))
      {
         worst = d;
         at = i;
      }
   }
   cases++;
   /* float roundings apart (GLSL's inverse() and the HLSL's own differ the
    * most, by 2e-4 under a panel's gamut compression): half a 10-bit code
    * of the HDR10 output at most; NaN never */
   if (!(worst <= 5e-4))
   {
      failures++;
      printf("hdr_hlsl: %s: pixel %u,%u channel %u: GLSL %g, HLSL %g\n", name,
            (at / 4) % W, (at / 4) / W, at % 4, glsl_out[at], hlsl_out[at]);
   }
   else if (getenv("HDR_HLSL_VERBOSE"))
      printf("hdr_hlsl: %s: %.3g\n", name, worst);
   if (against)
      against_case(name, o);
}

int main(int argc, char **argv)
{
   if (argc != 2 && !(argc == 4 && !strcmp(argv[2], "--against")))
   {
      fprintf(stderr, "usage: hdr_hlsl hlsl_ps.spv [--against d3d.bin]\n");
      return 2;
   }
   if (argc == 4)
   {
      against = fopen(argv[3], "rb");
      if (!against)
      {
         fprintf(stderr, "hdr_hlsl: cannot read %s\n", argv[3]);
         return 2;
      }
      against_out = (float*)malloc((size_t)W * H * 4 * sizeof(float));
   }
   setup(argv[1]);
   hdr_cases_each(run_case);
   if (validation_errors)
      failures++;
   printf("hdr_hlsl: %u cases, %u differ%s\n", cases, failures,
         validation_errors ? " (validation errors)" : "");
   return failures ? 1 : 0;
}
