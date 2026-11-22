/* The Vulkan context: the frontend's device and queue, what the device
 * can do, the memory heap, the descriptor pool and the pipeline cache.
 * One, as the frontend has one device for the core. */
#ifndef VK_CONTEXT_H
#define VK_CONTEXT_H

#include "vulkan.h"
#include "libretro_vulkan.h"
#include "rend/vram_budget.h"
#include "vk_heap.h"
#include "hdr_encode_vk.h"
#include "pso.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VENDOR_AMD 0x1022
#define VENDOR_ARM 0x13B5
#define VENDOR_INTEL 0x8086
#define VENDOR_NVIDIA 0x10DE
#define VENDOR_QUALCOMM 0x5143

typedef struct vk_context
{
	struct retro_hw_render_interface_vulkan *iface;
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	VkQueue queue;
	int swapChainSize;
	u32 width;
	u32 height;

	VkDeviceSize uniformBufferAlignment;
	VkDeviceSize storageBufferAlignment;
	u32 maxStorageBufferRange;
	VkDeviceSize maxMemoryAllocationSize;
	bool optimalTilingSupported565;
	bool optimalTilingSupported1555;
	bool optimalTilingSupported4444;

	/* what the device was made with (vk_create_device()) */
	bool samplerAnisotropy;
	bool storageImageExtendedFormats;
	f32 maxSamplerAnisotropy;
	bool dedicatedAllocationSupported;
	bool multiDrawIndirect;
	bool drawIndirectFirstInstance;
	bool drawIndirectCount;		/* VK_KHR_draw_indirect_count */
	bool sampleRateShading;
	bool postDepthCoverage;		/* VK_EXT_post_depth_coverage: SSAA's per-pixel lists (vulkan/shaders/oit_sample_depth.glsl) */
	bool shaderInt64;			/* enabled on the device when it has it */
	bool sampledImageArrayIndexing;	/* ...and this, shaderSampledImageArrayDynamicIndexing */
	bool nonUniformTextureIndexing;	/* ...and shaderSampledImageArrayNonUniformIndexing (VK_EXT_descriptor_indexing) */
	bool subgroupBallotCompute;	/* Vulkan 1.1's subgroup ballot in compute shaders */
	bool extendedDynamicState;
	bool extendedDynamicState3;
	bool pipelineCacheControl;

	vk_pso_dyn dynamicState;
	vk_pso_keys psoKeys;
	u32 savedBuilt;
	u32 maxDrawIndirectCount;
	PFN_vkCmdDrawIndexedIndirectCountKHR cmdDrawIndexedIndirectCount;
	u32 vendorID;
	u32 apiVersion;	/* the device's */
	VramBudget vramBudget;
	u32 ssaaSampleCounts;	/* VkSampleCountFlags */
	bool lazyMemory;
	VkFormat colorFormat;
	VkFormat depthFormat;

	VkDescriptorPool descriptorPool;
	VkPipelineCache pipelineCache;

	struct retro_vulkan_image retro_image;
	u32 presentedWidth;
	u32 presentedHeight;
	hdr_vk_t *hdr;
	VkFormat hdrFormat;
	const void *hdrPools[4];	/* the command pools the HDR encode has a ring for */

	/* the heap: everything the renderers allocate is from it */
	vk_heap_t heap;
	bool heapReady;
	u32 trimIn;		/* frames until the trim, 0 if none is due */
} vk_context;

extern vk_context vkc;

bool vkc_init(struct retro_hw_render_interface_vulkan *iface);
void vkc_term(void);
void vkc_set_window_size(u32 width, u32 height);
void vkc_present_frame(VkImage image, VkImageView view, s32 width, s32 height, bool hdr);
/* The frontend let go of the picture, and the queue waited out (a change
 * of renderer) */
void vkc_release_picture(void);
void vkc_wait_queue_idle(void);
void vkc_submit(u32 count, const VkCommandBuffer *buffers, VkFence fence);
void vkc_save_pipeline_cache(void);
void vkc_log_vram_usage(void);
u32 vkc_ssaa_samples(u32 requested, u32 perSampleKib, u32 fixedKib, bool transient, const char *what);
u32 vkc_depth_bytes(void);
/* TextureType: whether it can be an optimal tiling image */
bool vkc_format_supported(int textureType);
const char *vkc_driver_name(void);

static INLINE int vkc_current_image_index(void)
{
	return (int)vkc.iface->get_sync_index(vkc.iface->handle);
}

/* The HDR output's work, with the rings of @pool (a command pool, by its
 * address), @index the pool's frame in it, of @frames */
bool vkc_hdr_post_active(void);
bool vkc_hdr_sums_ready(void);
void vkc_hdr_prepare(const void *pool, unsigned index, unsigned frames, VkCommandBuffer cmd);
VkPipeline vkc_hdr_subpass_pipeline(VkRenderPass renderPass, u32 subpass, u32 samples);
void vkc_hdr_subpass_draw(const void *pool, unsigned index, VkCommandBuffer cmd, VkPipeline pipeline,
		VkImageView picture, VkImageView samples, u32 width, u32 height);
/* @cmd begun: the picture encoded into it; @image and @view then the
 * encoded one's */
bool vkc_hdr_encode(const void *pool, unsigned index, unsigned frames, VkCommandBuffer cmd,
		VkImage *image, VkImageView *view, u32 width, u32 height, VkImageView sums);

/* Memory, bound to @image or @buffer. False if there was none: the
 * driver's reason in vkc_heap_error(). */
bool vkc_heap_alloc_image(VkImage image, bool linear, bool hostVisible, bool transient, vk_heap_alloc_t *out);
bool vkc_heap_alloc_buffer(VkBuffer buffer, bool hostVisible, bool readBack, vk_heap_alloc_t *out);
VkResult vkc_heap_error(void);

/* After a change of size, memory left empty goes back in a while */
static INLINE void vkc_heap_trim_soon(void)
{
	vkc.trimIn = 32;
}

/* The negotiation interface's (libretro_vulkan.h) */
const VkApplicationInfo *VkGetApplicationInfo(void);
bool VkCreateDevice(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
		VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
		const char **required_device_extensions, unsigned num_required_device_extensions,
		const char **required_device_layers, unsigned num_required_device_layers,
		const VkPhysicalDeviceFeatures *required_features);
/* The same device, made through the frontend's wrapper (negotiation v2),
 * which brings its own extensions and features. */
bool VkCreateDevice2(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
		VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
		retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
