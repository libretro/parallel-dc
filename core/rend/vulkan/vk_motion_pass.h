/* The temporal upscaler's motion vector pass (rend/upscaler.h), for the
 * screen drawers: a render pass of its own after the frame's, over the
 * depth the frame left (loaded, tested, not written, kept for the
 * upscaler), into the vectors' image, which the upscaler reads as the
 * colour. The vectors are of the strips rend/upscaler.c matched with the
 * frame before's. The pipelines are made with the drawer's layout, whose
 * per-frame descriptor set (the vertex uniforms) is bound when
 * vk_motion_pass_record() is called. Its three pipelines are asked of the
 * pipeline thread when it is made (pso.h): the first record takes them,
 * or makes them there, and fails there as init did when the device
 * cannot. */
#ifndef VK_MOTION_PASS_H
#define VK_MOTION_PASS_H

#include "vk_image.h"
#include "pso.h"

#ifdef __cplusplus
extern "C" {
#endif

/* what is made for a size: the vectors' images and their framebuffers, one
 * a frame slot (one allocation, given to the command pool to destroy once
 * the frames that use it are done) */
typedef struct vk_motion_sized
{
	u32 count;
	vk_attachment *attachments;
	VkFramebuffer *framebuffers;
} vk_motion_sized;

typedef struct vk_motion_pass
{
	vk_pso_builder builder;
	VkExtent2D size;
	VkImageLayout depthLayout;
	VkRenderPass renderPass;
	PsoSlot pipelines[3];		/* by cull mode */
	/* what they are made of */
	VkPipelineLayout pipelineLayout;
	VkShaderModule vertexShader, fragmentShader;
	bool depthTest;
	vk_motion_sized *sized;
	vk_buffer *prevXy;			/* a frame slot each, as many as prevXyCount */
	u32 prevXyCount;
	bool recorded;
	/* the sampled-depth mode's own layout and sets */
	bool sampled;
	VkDescriptorSetLayout setLayout;
	VkPipelineLayout ownLayout;
	VkDescriptorPool descriptorPool;
	VkDescriptorSet *sets;
	u32 setCount;
	VkSampler depthSampler;
} vk_motion_pass;

void vk_motion_pass_construct(vk_motion_pass *m);
/* Made for a size and the frame's depth images (one a frame slot, which
 * the vectors' images are too), in the layout the frame leaves them in;
 * made again when any of that changes. False if it could not be. */
bool vk_motion_pass_init(vk_motion_pass *m, VkExtent2D size, VkFormat depthFormat, VkImageLayout depthLayout,
		const VkImageView *depthViews, u32 count, VkPipelineLayout pipelineLayout,
		VkShaderModule vertexShader, VkShaderModule fragmentShader, vk_cmd_pool *pool);
/* The same over a depth stored by the compute renderer, which is no
 * attachment but an image sampled (mv_depth.frag tests against it): the
 * pass has a layout of its own then, with the vertex uniforms
 * @uniformBuffers (one a frame slot, the drawer's) and the depth */
bool vk_motion_pass_init_sampled(vk_motion_pass *m, VkExtent2D size, const VkImageView *depthViews, u32 count,
		const VkBuffer *uniformBuffers, VkDeviceSize uniformBytes,
		VkShaderModule vertexShader, VkShaderModule fragmentShader, vk_cmd_pool *pool);
/* what was made goes once the frames that could use it have gone by (@pool
 * NULL: now) */
void vk_motion_pass_term(vk_motion_pass *m, vk_cmd_pool *pool);
/* Before the frame is drawn, under its jitter: whether this frame's
 * vectors are drawn (the frame's strips are needed: not a GPU-parsed
 * one); matches the strips and uploads where each vertex was */
bool vk_motion_pass_begin(vk_motion_pass *m, int image);
/* (the vertices and indices in @vertexBuffer, at their offsets); false if a
 * pipeline could not be made */
bool vk_motion_pass_record(vk_motion_pass *m, VkCommandBuffer cmd, int image, VkBuffer vertexBuffer, VkDeviceSize indexOffset,
		const VkRect2D *scissor, VkDeviceSize vertexOffset);
/* the vectors and the frame's depth, to the upscaler */
void vk_motion_pass_submit(vk_motion_pass *m, int image, VkImage depthImage, VkImageView depthView, VkFormat depthFormat);

#ifdef __cplusplus
}
#endif

#endif
