/* The HDR output's test host (vk_hdr.sh).
 *
 *   vk_hdr core content out.raw frames [key=value ...]
 *
 * Loads the core as a Vulkan frontend, serves each key=value as a core
 * option, makes the device through the core's negotiation interface (v2,
 * as RetroArch does), runs the content for the given number of frames and
 * writes the last picture the core handed over with set_image to out.raw:
 * "VKHR", then the Vulkan format, width and height (uint32 each), then the
 * pixels, 4 or 8 bytes each, a row after another.
 *
 *   VKH_FRONTEND=sdr     the frontend presents SDR: HDR pixel formats are
 *                        refused, as RetroArch refuses them with HDR off
 *   VKH_FRONTEND=hdr10   HDR10 is taken; scRGB is not offered
 *   VKH_FRONTEND=scrgb   scRGB is offered and taken
 *
 * The metadata answered for HDR: paper white 200 nits, peak 1000, no
 * black level, no panel primaries, Colour Boost accurate, passthrough.
 * VK_LAYER_KHRONOS_validation is enabled when installed, and any error it
 * reports fails the run, as does a picture of another format than the
 * pixel format the core asked for. VKH_DIR is the system/save directory.
 *
 *   VKH_UPSCALER=OWxOH:RWxRH   the frontend has a temporal upscaler
 *                        (RETRO_ENVIRONMENT_GET_HW_RENDER_UPSCALER_INTERFACE)
 *                        whose output is OW x OH and which asks for frames
 *                        of RW x RH. The run then fails unless the core
 *                        draws at that size and hands over, with its
 *                        frames, motion vectors and a depth of that size
 *                        with the jitter in range and a reset on the first;
 *                        the motion vectors are read back to out.raw.mv
 *                        ("VKHR", R16G16_SFLOAT) and must all be finite and
 *                        small, the disc's scene standing still; the depth
 *                        must have something drawn in it.
 *
 *   vk_hdr --compare sdr.raw hdr.raw
 *
 * Encodes the 24-bit picture on the CPU as the HDR output should have -
 * the 2.4 transfer, paper white, Rec.709 to Rec.2020 and PQ, or linear
 * scRGB - and fails where a channel of the HDR picture is outside what
 * that gives with the deband's grain either way and one output step of
 * slack (the Reinhard knee is the identity below paper white).
 *
 *   vk_hdr --same a.raw b.raw
 *
 * Fails unless both pictures are the same, format and bytes.
 *
 *   vk_hdr --post sdr.raw scrgb.raw [key=value ...]
 *
 * Runs the HDR post effects the options ask for on the 24-bit picture on
 * the CPU (tools/hdr/hdr_post_ref.c) and fails where the scRGB picture is
 * not what they make of it. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "libretro.h"
#include "libretro_vulkan.h"
#include <dlfcn.h>
#include "../hdr/hdr_post_ref.h"

#define VKH_SYNC_IMAGES 3

static char *opt_keys[64], *opt_vals[64];
static int nopts;
static struct retro_hw_render_callback hw;
static int have_hw;
static const struct retro_hw_render_context_negotiation_interface_vulkan *neg;
static int validation_errors;
static int frontend_hdr;                /* 0 sdr, 1 hdr10, 2 scrgb */
static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_hw_render_interface_vulkan iface;
static unsigned frame_no;

static struct retro_vulkan_image handed;
static int have_handed;
static unsigned frame_w, frame_h;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

/* the upscaler: the sizes VKH_UPSCALER names, and what the core handed over */
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
   if (!inputs->motion_vectors.image || !inputs->motion_vectors.view || !inputs->depth.image || !inputs->depth.view)
      bad = 1;
   if (inputs->motion_vectors.width != up_render_w || inputs->motion_vectors.height != up_render_h
         || inputs->depth.width != up_render_w || inputs->depth.height != up_render_h)
      bad = 1;
   if (inputs->motion_vectors.format != VK_FORMAT_R16G16_SFLOAT)
      bad = 1;
   if (inputs->depth.format != VK_FORMAT_D32_SFLOAT_S8_UINT && inputs->depth.format != VK_FORMAT_D24_UNORM_S8_UINT
         && inputs->depth.format != VK_FORMAT_D16_UNORM_S8_UINT)
      bad = 1;
   if (inputs->jitter_x < -0.5f || inputs->jitter_x > 0.5f || inputs->jitter_y < -0.5f || inputs->jitter_y > 0.5f)
      bad = 1;
   if (!(inputs->flags & RETRO_HW_UPSCALER_DEPTH_INVERTED) || (inputs->flags & RETRO_HW_UPSCALER_MV_OUTPUT_RES))
      bad = 1;
   if (up_count == 0)
      up_first_reset = (inputs->flags & RETRO_HW_UPSCALER_RESET) != 0;
   if (bad)
   {
      fprintf(stderr, "vk_hdr: upscaler inputs %u: mv %ux%u format %u, depth %ux%u format %u, jitter %f %f, flags %x\n",
            up_count, inputs->motion_vectors.width, inputs->motion_vectors.height, inputs->motion_vectors.format,
            inputs->depth.width, inputs->depth.height, inputs->depth.format, inputs->jitter_x, inputs->jitter_y, inputs->flags);
      up_bad++;
   }
   up_count++;
}

static const struct retro_hw_render_upscaler_interface up_iface = {
   RETRO_HW_RENDER_UPSCALER_INTERFACE_VERSION, NULL, up_get_request, up_set_inputs
};

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("VKH_VERBOSE"))
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

static bool environ_cb(unsigned cmd, void *data)
{
   const char *dir = getenv("VKH_DIR") ? getenv("VKH_DIR") : ".";
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
         *(unsigned*)data = RETRO_HW_CONTEXT_VULKAN;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         const struct retro_hw_render_callback *cb = (const struct retro_hw_render_callback*)data;
         if (cb->context_type != RETRO_HW_CONTEXT_VULKAN)
            return false;
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         *(const struct retro_hw_render_interface**)data = (const struct retro_hw_render_interface*)&iface;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_UPSCALER_INTERFACE:
         if (!up_render_w)
            return false;
         *(const struct retro_hw_render_upscaler_interface**)data = &up_iface;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
      {
         const struct retro_hw_render_context_negotiation_interface *n =
            (const struct retro_hw_render_context_negotiation_interface*)data;
         if (n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
            return false;
         neg = (const struct retro_hw_render_context_negotiation_interface_vulkan*)data;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
      {
         struct retro_hw_render_context_negotiation_interface *n =
            (struct retro_hw_render_context_negotiation_interface*)data;
         n->interface_version = n->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN ? 2 : 0;
         return true;
      }
      default:
         return false;
   }
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t p)
{
   (void)p;
   if (d == RETRO_HW_FRAME_BUFFER_VALID)
   {
      frame_w = w;
      frame_h = h;
   }
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

/* ---- the frontend's side of the HW interface ---- */

static void set_image(void *h, const struct retro_vulkan_image *image, uint32_t n,
      const VkSemaphore *sem, uint32_t family)
{
   (void)h; (void)n; (void)sem; (void)family;
   if (image == NULL)
   {
      have_handed = 0;
      return;
   }
   handed      = *image;
   have_handed = 1;
}
static uint32_t get_sync_index(void *h) { (void)h; return frame_no % VKH_SYNC_IMAGES; }
static uint32_t get_sync_index_mask(void *h) { (void)h; return (1u << VKH_SYNC_IMAGES) - 1; }
static void set_command_buffers(void *h, uint32_t n, const VkCommandBuffer *c) { (void)h; (void)n; (void)c; }
static void wait_sync_index(void *h)
{
   PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)gdpa(iface.device, "vkQueueWaitIdle");
   (void)h;
   wait(iface.queue);
}
static void lock_queue(void *h) { (void)h; }
static void unlock_queue(void *h) { (void)h; }
static void set_signal_semaphore(void *h, VkSemaphore s) { (void)h; (void)s; }

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
      VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u)
{
   (void)type; (void)u;
   if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
   {
      validation_errors++;
      fprintf(stderr, "vk_hdr: validation: %s\n", d->pMessage);
   }
   return VK_FALSE;
}

static int finish(int code)
{
   fflush(stdout);
   fflush(stderr);
   _Exit(code);
   return code;
}

/* ---- the picture back ---- */

static int read_back_image(VkImage image, VkImageLayout layout, VkImageAspectFlags aspect, VkFormat format,
      uint32_t w, uint32_t h, uint32_t bpp, const char *out);

static int read_back(const char *out)
{
   return read_back_image(handed.create_info.image, handed.image_layout, VK_IMAGE_ASPECT_COLOR_BIT,
         handed.create_info.format, frame_w, frame_h,
         handed.create_info.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4, out);
}

/* (a depth image's depth, as its format lays a pixel's out: 4 bytes for
 * D32 and D24, 2 for D16) */
static int read_back_image(VkImage image, VkImageLayout layout, VkImageAspectFlags aspect, VkFormat format,
      uint32_t w, uint32_t h, uint32_t bpp, const char *out)
{
   PFN_vkCreateBuffer p_vkCreateBuffer = (PFN_vkCreateBuffer)gdpa(iface.device, "vkCreateBuffer");
   PFN_vkGetBufferMemoryRequirements p_req = (PFN_vkGetBufferMemoryRequirements)gdpa(iface.device, "vkGetBufferMemoryRequirements");
   PFN_vkAllocateMemory p_alloc = (PFN_vkAllocateMemory)gdpa(iface.device, "vkAllocateMemory");
   PFN_vkBindBufferMemory p_bind = (PFN_vkBindBufferMemory)gdpa(iface.device, "vkBindBufferMemory");
   PFN_vkMapMemory p_map = (PFN_vkMapMemory)gdpa(iface.device, "vkMapMemory");
   PFN_vkCreateCommandPool p_pool = (PFN_vkCreateCommandPool)gdpa(iface.device, "vkCreateCommandPool");
   PFN_vkAllocateCommandBuffers p_cmd = (PFN_vkAllocateCommandBuffers)gdpa(iface.device, "vkAllocateCommandBuffers");
   PFN_vkBeginCommandBuffer p_begin = (PFN_vkBeginCommandBuffer)gdpa(iface.device, "vkBeginCommandBuffer");
   PFN_vkEndCommandBuffer p_end = (PFN_vkEndCommandBuffer)gdpa(iface.device, "vkEndCommandBuffer");
   PFN_vkCmdPipelineBarrier p_barrier = (PFN_vkCmdPipelineBarrier)gdpa(iface.device, "vkCmdPipelineBarrier");
   PFN_vkCmdCopyImageToBuffer p_copy = (PFN_vkCmdCopyImageToBuffer)gdpa(iface.device, "vkCmdCopyImageToBuffer");
   PFN_vkQueueSubmit p_submit = (PFN_vkQueueSubmit)gdpa(iface.device, "vkQueueSubmit");
   PFN_vkQueueWaitIdle p_wait = (PFN_vkQueueWaitIdle)gdpa(iface.device, "vkQueueWaitIdle");
   PFN_vkGetPhysicalDeviceMemoryProperties p_props = (PFN_vkGetPhysicalDeviceMemoryProperties)
      gipa(iface.instance, "vkGetPhysicalDeviceMemoryProperties");
   VkPhysicalDeviceMemoryProperties props;
   VkBufferCreateInfo bci;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo mai;
   VkCommandPoolCreateInfo cpi;
   VkCommandBufferAllocateInfo cbi;
   VkCommandBufferBeginInfo begin;
   VkImageMemoryBarrier b;
   VkBufferImageCopy copy;
   VkSubmitInfo submit;
   VkBuffer buffer;
   VkDeviceMemory memory;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   void *mapped;
   uint32_t i, header[4];
   FILE *f;

   memset(&bci, 0, sizeof(bci));
   bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bci.size  = (VkDeviceSize)w * h * bpp;
   bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   if (p_vkCreateBuffer(iface.device, &bci, NULL, &buffer) != VK_SUCCESS)
      return 0;
   p_req(iface.device, buffer, &req);
   p_props(iface.gpu, &props);
   memset(&mai, 0, sizeof(mai));
   mai.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize = req.size;
   for (i = 0; i < props.memoryTypeCount; i++)
      if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags
            & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         break;
   mai.memoryTypeIndex = i;
   if (i == props.memoryTypeCount || p_alloc(iface.device, &mai, NULL, &memory) != VK_SUCCESS)
      return 0;
   p_bind(iface.device, buffer, memory, 0);
   memset(&cpi, 0, sizeof(cpi));
   cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpi.queueFamilyIndex = iface.queue_index;
   p_pool(iface.device, &cpi, NULL, &pool);
   memset(&cbi, 0, sizeof(cbi));
   cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbi.commandPool        = pool;
   cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbi.commandBufferCount = 1;
   p_cmd(iface.device, &cbi, &cmd);
   memset(&begin, 0, sizeof(begin));
   begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   p_begin(cmd, &begin);
   memset(&b, 0, sizeof(b));
   b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout           = layout;
   b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image               = image;
   b.subresourceRange.aspectMask = aspect == VK_IMAGE_ASPECT_DEPTH_BIT
                                 ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : aspect;
   b.subresourceRange.levelCount = 1;
   b.subresourceRange.layerCount = 1;
   p_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   memset(&copy, 0, sizeof(copy));
   copy.imageSubresource.aspectMask = aspect;
   copy.imageSubresource.layerCount = 1;
   copy.imageExtent.width           = w;
   copy.imageExtent.height          = h;
   copy.imageExtent.depth           = 1;
   p_copy(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
   b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout     = layout;
   p_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   p_end(cmd);
   memset(&submit, 0, sizeof(submit));
   submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit.commandBufferCount = 1;
   submit.pCommandBuffers    = &cmd;
   p_submit(iface.queue, 1, &submit, VK_NULL_HANDLE);
   p_wait(iface.queue);
   p_map(iface.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
   if (!(f = fopen(out, "wb")))
      return 0;
   header[0] = 0x52484b56u;    /* "VKHR" */
   header[1] = (uint32_t)format;
   header[2] = w;
   header[3] = h;
   fwrite(header, sizeof(header), 1, f);
   fwrite(mapped, 1, (size_t)w * h * bpp, f);
   fclose(f);
   return 1;
}

/* ---- the upscaler's inputs back ---- */

static float half_to_float(uint16_t h)
{
   const unsigned s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
   float v;
   if (e == 0)
      v = (float)m / 1024.0f / 16384.0f;
   else if (e == 31)
      v = m ? NAN : INFINITY;
   else
      v = (1.0f + (float)m / 1024.0f) * powf(2.0f, (float)e - 15.0f);
   return s ? -v : v;
}

/* The motion vectors to @out, checked: finite and under a pixel
 * everywhere, the scene standing still; the depth with something in it */
static int check_upscaler_inputs(const char *out)
{
   char path[1024];
   FILE *f;
   uint32_t header[4], n, i, drawn = 0, moving = 0, nonfinite = 0;
   uint16_t *mv;
   uint32_t *depth;
   const uint32_t depth_bpp = up_inputs.depth.format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;

   snprintf(path, sizeof(path), "%s.mv", out);
   if (!read_back_image((VkImage)(uintptr_t)up_inputs.motion_vectors.image, (VkImageLayout)up_inputs.motion_vectors.state,
            VK_IMAGE_ASPECT_COLOR_BIT, (VkFormat)up_inputs.motion_vectors.format, up_render_w, up_render_h, 4, path))
      return 0;
   if (!(f = fopen(path, "rb")))
      return 0;
   n = up_render_w * up_render_h;
   mv = (uint16_t*)malloc((size_t)n * 4);
   if (fread(header, sizeof(header), 1, f) != 1 || fread(mv, 4, n, f) != n)
   {
      fclose(f);
      return 0;
   }
   fclose(f);
   for (i = 0; i < n * 2; i++)
   {
      const float v = half_to_float(mv[i]);
      if (v != v || fabsf(v) == INFINITY)
         nonfinite++;
      else if (fabsf(v) > 1.0f)
         moving++;
   }
   free(mv);
   if (nonfinite || moving)
   {
      fprintf(stderr, "vk_hdr: motion vectors: %u not finite, %u over a pixel, in a scene standing still\n", nonfinite, moving);
      return 0;
   }

   snprintf(path, sizeof(path), "%s.depth", out);
   if (!read_back_image((VkImage)(uintptr_t)up_inputs.depth.image, (VkImageLayout)up_inputs.depth.state,
            VK_IMAGE_ASPECT_DEPTH_BIT, (VkFormat)up_inputs.depth.format, up_render_w, up_render_h, depth_bpp, path))
      return 0;
   if (!(f = fopen(path, "rb")))
      return 0;
   depth = (uint32_t*)calloc(n, 4);
   if (fread(header, sizeof(header), 1, f) != 1 || fread(depth, depth_bpp, n, f) != n)
   {
      fclose(f);
      return 0;
   }
   fclose(f);
   for (i = 0; i < n * depth_bpp / 4; i++)
      if (depth[i] != 0)
         drawn++;
   free(depth);
   if (drawn < n / 8)
   {
      fprintf(stderr, "vk_hdr: the depth has %u of %u pixels drawn\n", drawn, n);
      return 0;
   }
   return 1;
}

/* ---- comparing ---- */

typedef struct picture
{
   uint32_t format, w, h;
   unsigned char *px;
} picture_t;

static int picture_load(const char *path, picture_t *p)
{
   FILE *f = fopen(path, "rb");
   uint32_t header[4];
   size_t n;
   p->px = NULL;
   if (!f)
      return 0;
   if (fread(header, sizeof(header), 1, f) != 1 || header[0] != 0x52484b56u)
   {
      fclose(f);
      return 0;
   }
   p->format = header[1];
   p->w      = header[2];
   p->h      = header[3];
   n         = (size_t)p->w * p->h * (p->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4);
   p->px     = (unsigned char*)malloc(n);
   if (!p->px || fread(p->px, 1, n, f) != n)
   {
      free(p->px);
      p->px = NULL;
      fclose(f);
      return 0;
   }
   fclose(f);
   return 1;
}

/* @1 and @2 loaded, run through @3, and freed */
static int with_pictures(const char *a_path, const char *b_path,
      int (*fn)(const picture_t *a, const picture_t *b))
{
   picture_t a, b;
   int rc = 2;
   b.px = NULL;
   if (!picture_load(a_path, &a) || !picture_load(b_path, &b))
      fprintf(stderr, "vk_hdr: cannot read the pictures\n");
   else
      rc = fn(&a, &b);
   free(a.px);
   free(b.px);
   return rc;
}

static double half_to_double(uint16_t h)
{
   int e = (h >> 10) & 31, m = h & 1023;
   double v = e == 0 ? ldexp(m, -24) : ldexp(1024 + m, e - 25);
   return (h & 0x8000) ? -v : v;
}

static double pq_encode(double nits)
{
   const double m1 = 2610.0 / 16384.0, m2 = (2523.0 / 4096.0) * 128.0;
   const double c1 = 3424.0 / 4096.0, c2 = (2413.0 / 4096.0) * 32.0, c3 = (2392.0 / 4096.0) * 32.0;
   double y = nits / 10000.0, ym;
   if (y <= 0.0)
      return 0.0;
   if (y > 1.0)
      y = 1.0;
   ym = pow(y, m1);
   return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

/* What the encode makes of a 24-bit channel value g (0 to 1, and a
 * little past it with the deband's grain), in paper-white units: the 2.4
 * transfer, and nothing more below paper white */
static double expect_linear(double g)
{
   if (g < 0.0)
      g = 0.0;
   return pow(g, 2.4);
}

/* Every channel within what the 24-bit picture's value, the deband's
 * grain (+-1 8-bit step, before the transfer) taken either way, can come
 * to: the bounds encoded as the shader encodes, and one output step of
 * slack. The 709 -> 2020 rotation has no negative term, so the bounds of
 * the rotated colour are those of the bounds. */
static int compare_pictures(const picture_t *sp_, const picture_t *hp_)
{
   static const double to2020[9] = {
      0.6274040, 0.3292820, 0.0433136,
      0.0690970, 0.9195400, 0.0113612,
      0.0163916, 0.0880132, 0.8955950 };
   const picture_t s = *sp_, h = *hp_;
   uint32_t x, y;
   unsigned long bad = 0;
   double worst = 0.0;
   const double paper = 200.0;
   if (s.w != h.w || s.h != h.h)
   {
      fprintf(stderr, "vk_hdr: %ux%u against %ux%u\n", s.w, s.h, h.w, h.h);
      return 1;
   }
   if (h.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && h.format != VK_FORMAT_R16G16B16A16_SFLOAT)
   {
      fprintf(stderr, "vk_hdr: the HDR picture is format %u\n", h.format);
      return 1;
   }
   for (y = 0; y < s.h; y++)
      for (x = 0; x < s.w; x++)
      {
         const unsigned char *sp = s.px + ((size_t)y * s.w + x) * 4;
         double lo[3], hi[3], got, a, b, over;
         int c, k;
         for (c = 0; c < 3; c++)
         {
            lo[c] = expect_linear((sp[c] - 1) / 255.0);
            hi[c] = expect_linear((sp[c] + 1) / 255.0);
         }
         for (c = 0; c < 3; c++)
         {
            if (h.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
            {
               uint32_t v = ((const uint32_t*)h.px)[(size_t)y * h.w + x];
               double l = 0.0, u = 0.0;
               for (k = 0; k < 3; k++)
               {
                  l += to2020[c * 3 + k] * lo[k];
                  u += to2020[c * 3 + k] * hi[k];
               }
               got = (double)((v >> (10 * c)) & 1023);
               a   = floor(pq_encode(l * paper) * 1023.0) - 1.0;
               b   = ceil(pq_encode(u * paper) * 1023.0) + 1.0;
            }
            else
            {
               const uint16_t *hp = (const uint16_t*)h.px + ((size_t)y * h.w + x) * 4;
               got = half_to_double(hp[c]);
               a   = lo[c] * paper / 80.0 * (1.0 - 1.0 / 1024.0) - 1e-6;
               b   = hi[c] * paper / 80.0 * (1.0 + 1.0 / 1024.0) + 1e-6;
            }
            over = got < a ? a - got : got > b ? got - b : 0.0;
            if (over > 0.0)
            {
               if (bad < 4)
                  fprintf(stderr, "vk_hdr: (%u,%u) channel %d is %g, not in [%g, %g] (24-bit %u)\n",
                        x, y, c, got, a, b, sp[c]);
               bad++;
               if (over > worst)
                  worst = over;
            }
         }
      }
   printf("vk_hdr: %ux%u %s, %lu channels outside what the 24-bit picture encodes to\n",
         s.w, s.h, h.format == VK_FORMAT_R16G16B16A16_SFLOAT ? "scRGB" : "HDR10", bad);
   return bad ? 1 : 0;
}

static int same_pictures(const picture_t *a, const picture_t *b)
{
   if (a->format != b->format || a->w != b->w || a->h != b->h
         || memcmp(a->px, b->px, (size_t)a->w * a->h * (a->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4)))
   {
      fprintf(stderr, "vk_hdr: the pictures differ\n");
      return 1;
   }
   printf("vk_hdr: the same picture, %ux%u format %u\n", a->w, a->h, a->format);
   return 0;
}

/* The scRGB picture (@h) of an SSAA frame against its 24-bit one (@s),
 * in the 24-bit picture's steps (the reference knee is the identity below
 * paper white, and 2.4 the transfer): each channel decoded is within half
 * a step of the byte the resolve rounded it to (and a half float's
 * rounding) - what was encoded is the samples' exact average, which no
 * dither moved - and some are off the steps, which a byte cannot be. */
static int exact_pictures(const picture_t *s, const picture_t *h)
{
   const double paper = 200.0, tol = 0.5 + 0.12;
   unsigned long off = 0, n = 0;
   double worst = 0.0;
   uint32_t i;
   int c;
   if (s->w != h->w || s->h != h->h || h->format != VK_FORMAT_R16G16B16A16_SFLOAT)
   {
      fprintf(stderr, "vk_hdr: --exact wants a scRGB picture the size of the 24-bit one\n");
      return 2;
   }
   for (i = 0; i < s->w * s->h; i++)
      for (c = 0; c < 3; c++)
      {
         const double got = half_to_double(((const uint16_t*)h->px)[i * 4 + c]) * 80.0 / paper;
         const double e   = 255.0 * pow(got > 0.0 ? got : 0.0, 1.0 / 2.4);
         const double d   = fabs(e - (double)s->px[i * 4 + c]);
         n++;
         if (d > worst)
            worst = d;
         if (fabs(e - floor(e + 0.5)) > 0.15)
            off++;
      }
   printf("vk_hdr: %lu channels, %lu off the 24-bit steps, at most %.3f of a step from the 24-bit picture\n",
         n, off, worst);
   if (worst > tol)
   {
      fprintf(stderr, "vk_hdr: a channel is %.3f of a step from the 24-bit picture: dithered, or not its resolve\n", worst);
      return 1;
   }
   if (off < 100)
   {
      fprintf(stderr, "vk_hdr: %lu channels off the 24-bit steps: what was encoded is the bytes, not the samples\n", off);
      return 1;
   }
   return 0;
}

static int compare(const char *sdr_path, const char *hdr_path)
{
   return with_pictures(sdr_path, hdr_path, compare_pictures);
}

static int same(const char *a_path, const char *b_path)
{
   return with_pictures(a_path, b_path, same_pictures);
}

/* ---- running the core ---- */

typedef void (*set_env_t)(retro_environment_t);
typedef void (*set_video_t)(retro_video_refresh_t);
typedef void (*set_audio_t)(retro_audio_sample_t);
typedef void (*set_audio_batch_t)(retro_audio_sample_batch_t);
typedef void (*set_poll_t)(retro_input_poll_t);
typedef void (*set_state_t)(retro_input_state_t);
typedef void (*init_t)(void);
typedef void (*run_t)(void);
typedef bool (*load_game_t)(const struct retro_game_info*);
#define CORE_FN(type, name) type name = (type)dlsym(core, #name)

/* --post: the options as key=value */
static int post(const char *sdr_path, const char *scrgb_path, int n, char **opts)
{
   picture_t   s, h;
   const char *keys[64], *vals[64];
   int         i, k = 0, rc = 2;
   char        buf[64][128];
   h.px = NULL;
   for (i = 0; i < n && k < 64; i++)
   {
      const char *eq = strchr(opts[i], '=');
      if (!eq || (size_t)(eq - opts[i]) >= sizeof(buf[0]))
         continue;
      memcpy(buf[k], opts[i], (size_t)(eq - opts[i]));
      buf[k][eq - opts[i]] = '\0';
      keys[k] = buf[k];
      vals[k++] = eq + 1;
   }
   if (!picture_load(sdr_path, &s) || !picture_load(scrgb_path, &h))
      fprintf(stderr, "vk_hdr: cannot read the pictures\n");
   else if (s.w != h.w || s.h != h.h || h.format != VK_FORMAT_R16G16B16A16_SFLOAT)
      fprintf(stderr, "vk_hdr: --post wants a scRGB picture the size of the 24-bit one\n");
   else
      rc = hdr_post_ref_check(s.px, s.w, s.h, (const uint16_t*)h.px, half_to_double, keys, vals, k);
   free(s.px);
   free(h.px);
   return rc;
}

int main(int argc, char **argv)
{
   void *core, *vk;
   struct retro_game_info game;
   VkApplicationInfo app;
   VkInstanceCreateInfo ici;
   VkDebugUtilsMessengerCreateInfoEXT dci;
   VkLayerProperties layers[64];
   uint32_t nlayers = 64, ngpu = 1, i;
   const char *layer = "VK_LAYER_KHRONOS_validation";
   const char *iexts[2];
   uint32_t niexts = 0;
   int validation = 0;
   VkInstance instance;
   VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
   VkPhysicalDevice gpu;
   struct retro_vulkan_context ctx;
   PFN_vkCreateInstance p_vkCreateInstance;
   PFN_vkEnumerateInstanceLayerProperties p_vkEnumerateInstanceLayerProperties;
   PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
   unsigned frames;
   const char *fe = getenv("VKH_FRONTEND");
   VkFormat expect;

   if (argc == 4 && !strcmp(argv[1], "--compare"))
      return compare(argv[2], argv[3]);
   if (argc == 4 && !strcmp(argv[1], "--exact"))
      return with_pictures(argv[2], argv[3], exact_pictures);
   if (argc == 4 && !strcmp(argv[1], "--same"))
      return same(argv[2], argv[3]);
   if (argc >= 4 && !strcmp(argv[1], "--post"))
      return post(argv[2], argv[3], argc - 4, argv + 4);
   if (argc < 5)
   {
      fprintf(stderr, "usage: vk_hdr core content out.raw frames [key=value ...]\n");
      return 2;
   }
   frontend_hdr = fe && !strcmp(fe, "scrgb") ? 2 : fe && !strcmp(fe, "hdr10") ? 1 : 0;
   frames = (unsigned)atoi(argv[4]);
   for (i = 5; i < (uint32_t)argc && nopts < 64; i++)
   {
      char *eq = strchr(argv[i], '=');
      if (!eq)
         continue;
      *eq = '\0';
      opt_keys[nopts] = argv[i];
      opt_vals[nopts++] = eq + 1;
   }

   if (!(core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)))
   {
      fprintf(stderr, "vk_hdr: cannot load %s\n", argv[1]);
      return 2;
   }
   {
      CORE_FN(set_env_t, retro_set_environment);
      CORE_FN(set_video_t, retro_set_video_refresh);
      CORE_FN(set_audio_t, retro_set_audio_sample);
      CORE_FN(set_audio_batch_t, retro_set_audio_sample_batch);
      CORE_FN(set_poll_t, retro_set_input_poll);
      CORE_FN(set_state_t, retro_set_input_state);
      CORE_FN(init_t, retro_init);
      CORE_FN(load_game_t, retro_load_game);
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
         fprintf(stderr, "vk_hdr: retro_load_game failed\n");
         return finish(2);
      }
   }
   if (getenv("VKH_UPSCALER") && sscanf(getenv("VKH_UPSCALER"), "%ux%u:%ux%u",
            &up_out_w, &up_out_h, &up_render_w, &up_render_h) != 4)
   {
      fprintf(stderr, "vk_hdr: VKH_UPSCALER is OWxOH:RWxRH\n");
      return finish(2);
   }
   if (!have_hw || !neg || neg->interface_version < 2 || !neg->create_device2)
   {
      fprintf(stderr, "vk_hdr: the core did not ask for Vulkan with negotiation v2\n");
      return finish(1);
   }

   if (!(vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)))
   {
      fprintf(stderr, "vk_hdr: no Vulkan loader\n");
      return finish(2);
   }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
   p_vkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   p_vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)
      gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
   if (p_vkEnumerateInstanceLayerProperties(&nlayers, layers) >= 0)
      for (i = 0; i < nlayers; i++)
         if (!strcmp(layers[i].layerName, layer))
            validation = 1;
   iexts[niexts++] = "VK_KHR_surface";
   if (validation)
      iexts[niexts++] = "VK_EXT_debug_utils";
   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.apiVersion = VK_API_VERSION_1_1;
   if (neg->get_application_info && neg->get_application_info())
      app = *neg->get_application_info();
   if (app.apiVersion < VK_API_VERSION_1_1)
      app.apiVersion = VK_API_VERSION_1_1;
   memset(&dci, 0, sizeof(dci));
   dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
   dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
   dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
   dci.pfnUserCallback = debug_cb;
   memset(&ici, 0, sizeof(ici));
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &app;
   ici.enabledExtensionCount = niexts;
   ici.ppEnabledExtensionNames = iexts;
   if (validation)
   {
      ici.pNext = &dci;
      ici.enabledLayerCount = 1;
      ici.ppEnabledLayerNames = &layer;
   }
   if (p_vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
   {
      fprintf(stderr, "vk_hdr: vkCreateInstance failed\n");
      return finish(2);
   }
   if (validation)
   {
      PFN_vkCreateDebugUtilsMessengerEXT create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)
         gipa(instance, "vkCreateDebugUtilsMessengerEXT");
      if (!create_messenger || create_messenger(instance, &dci, NULL, &messenger) != VK_SUCCESS)
         return finish(2);
   }
   p_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
   if (p_vkEnumeratePhysicalDevices(instance, &ngpu, &gpu) < 0 || !ngpu)
   {
      fprintf(stderr, "vk_hdr: no GPU\n");
      return finish(2);
   }
   memset(&ctx, 0, sizeof(ctx));
   if (!neg->create_device2(&ctx, instance, gpu, VK_NULL_HANDLE, gipa, NULL, NULL))
   {
      fprintf(stderr, "vk_hdr: create_device2 failed\n");
      return finish(1);
   }
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");

   memset(&iface, 0, sizeof(iface));
   iface.interface_type        = RETRO_HW_RENDER_INTERFACE_VULKAN;
   iface.interface_version     = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
   iface.instance              = instance;
   iface.gpu                   = gpu;
   iface.device                = ctx.device;
   iface.get_device_proc_addr  = gdpa;
   iface.get_instance_proc_addr = gipa;
   iface.queue                 = ctx.queue;
   iface.queue_index           = ctx.queue_family_index;
   iface.set_image             = set_image;
   iface.get_sync_index        = get_sync_index;
   iface.get_sync_index_mask   = get_sync_index_mask;
   iface.set_command_buffers   = set_command_buffers;
   iface.wait_sync_index       = wait_sync_index;
   iface.lock_queue            = lock_queue;
   iface.unlock_queue          = unlock_queue;
   iface.set_signal_semaphore  = set_signal_semaphore;
   hw.context_reset();
   {
      CORE_FN(run_t, retro_run);
      for (frame_no = 0; frame_no < frames; frame_no++)
         retro_run();
   }
   if (!have_handed || frame_w == 0)
   {
      fprintf(stderr, "vk_hdr: no picture was handed over\n");
      return finish(1);
   }
   expect = pix_fmt == RETRO_PIXEL_FORMAT_HDR10_2101010 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
          : pix_fmt == RETRO_PIXEL_FORMAT_SCRGB_RGBA16F ? VK_FORMAT_R16G16B16A16_SFLOAT
          : VK_FORMAT_R8G8B8A8_UNORM;
   if (handed.create_info.format != expect)
   {
      fprintf(stderr, "vk_hdr: the pixel format is %d, the picture handed over format %d\n",
            (int)pix_fmt, (int)handed.create_info.format);
      return finish(1);
   }
   if (!read_back(argv[3]))
   {
      fprintf(stderr, "vk_hdr: the picture could not be read back\n");
      return finish(2);
   }
   if (up_render_w)
   {
      if (frame_w != up_render_w || frame_h != up_render_h)
      {
         fprintf(stderr, "vk_hdr: the upscaler asked for %ux%u, the core drew %ux%u\n",
               up_render_w, up_render_h, frame_w, frame_h);
         return finish(1);
      }
      /* (the disc renders three frames in four through the tile accelerator) */
      if (up_count < frames / 2 || up_bad || !up_first_reset)
      {
         fprintf(stderr, "vk_hdr: upscaler inputs for %u of %u frames, %u wrong, first reset %d\n",
               up_count, frames, up_bad, up_first_reset);
         return finish(1);
      }
      if (!check_upscaler_inputs(argv[3]))
         return finish(1);
   }
   if (validation_errors)
      return finish(1);
   printf("vk_hdr: %u frames, pixel format %d, picture %ux%u format %d%s%s\n", frames, (int)pix_fmt,
         frame_w, frame_h, (int)handed.create_info.format, validation ? ", validation clean" : "",
         up_render_w ? ", upscaler inputs checked" : "");
   return finish(0);
}
