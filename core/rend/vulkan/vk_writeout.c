/* The chip's write-out for the Vulkan raster renderers: see writeout.h */
#include <stdlib.h>
#include <string.h>
#include "writeout.h"

/* a slot of words a picture: the frame's PVRC_F_WIDTH to PVRC_F_FLAGS, at an offset any device takes */
#define SLOT_BYTES 256
typedef char slot_holds_the_words[PVRC_F_FLAGS < SLOT_BYTES / 4 && PVRC_F_WIDTH < SLOT_BYTES / 4
		&& PVRC_F_HEIGHT < SLOT_BYTES / 4 ? 1 : -1];

static VkPipeline writeout_build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	const vk_writeout *w = (const vk_writeout *)self;
	VkComputePipelineCreateInfo info;
	(void)key;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = w->module;
	info.stage.pName = "main";
	info.layout = w->pipelineLayout;
	return vk_pso_create_compute(vkc.device, cache, &info);
}

void vk_writeout_construct(vk_writeout *w)
{
	memset(w, 0, sizeof(*w));
	w->builder.build = writeout_build;
	retro_atomic_int_init(&w->builder.generation, 0);
	PsoOwnInit(&w->builder, &w->slot, 1);
}

/* what the pictures of a size have */
static void drop_sized(vk_writeout *w)
{
	u32 i;
	if (w->overlayFramebuffers != NULL)
		for (i = 0; i < w->count; i++)
			if (w->overlayFramebuffers[i] != VK_NULL_HANDLE)
				vkDestroyFramebuffer(vkc.device, w->overlayFramebuffers[i], NULL);
	free(w->overlayFramebuffers);
	w->overlayFramebuffers = NULL;
	if (w->overlayPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(vkc.device, w->overlayPass, NULL);
	w->overlayPass = VK_NULL_HANDLE;
	free(w->sets);
	w->sets = NULL;
	w->count = 0;
	/* (the sets with it) */
	if (w->pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(vkc.device, w->pool, NULL);
	w->pool = VK_NULL_HANDLE;
	vk_buffer_free(&w->words);
	w->mapped = NULL;
}

void vk_writeout_term(vk_writeout *w)
{
	drop_sized(w);
	PsoOwnDrop(&w->builder, &w->slot, 1, vkc.device);
	if (w->module != VK_NULL_HANDLE)
		vkDestroyShaderModule(vkc.device, w->module, NULL);
	if (w->pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vkc.device, w->pipelineLayout, NULL);
	if (w->setLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, w->setLayout, NULL);
	w->module = VK_NULL_HANDLE;
	w->pipelineLayout = VK_NULL_HANDLE;
	w->setLayout = VK_NULL_HANDLE;
}

static bool make_layouts(vk_writeout *w)
{
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo setInfo;
	VkPipelineLayoutCreateInfo layoutInfo;
	VkShaderModuleCreateInfo moduleInfo;
	PsoSlot *one = &w->slot;
	size_t size = 0;

	/* (the kernel's bindings: its frame's words, 0, and the picture, 3) */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 3;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 2;
	setInfo.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &w->setLayout) != VK_SUCCESS)
	{
		w->setLayout = VK_NULL_HANDLE;
		return false;
	}
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &w->setLayout;
	if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &w->pipelineLayout) != VK_SUCCESS)
	{
		w->pipelineLayout = VK_NULL_HANDLE;
		return false;
	}
	memset(&moduleInfo, 0, sizeof(moduleInfo));
	moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	moduleInfo.pCode = vk_compute_kernel(PVRC_K_YFILTER, &size);
	moduleInfo.codeSize = size;
	if (vkCreateShaderModule(vkc.device, &moduleInfo, NULL, &w->module) != VK_SUCCESS)
	{
		w->module = VK_NULL_HANDLE;
		return false;
	}
	vk_pso_worker_build_soon(&one, 1, vkc.pipelineCache);
	return true;
}

/* What goes over the picture: a render pass that loads it as the pass
 * left it, and leaves it as the frame's render pass does */
static bool make_overlay_pass(vk_writeout *w)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference colorReference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo info;

	memset(&attachment, 0, sizeof(attachment));
	attachment.format = vkc.colorFormat;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	colorReference.attachment = 0;
	colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	memset(dependencies, 0, sizeof(dependencies));
	/* (after record()'s barrier, whose second half it follows on from) */
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &attachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	if (vkCreateRenderPass(vkc.device, &info, NULL, &w->overlayPass) != VK_SUCCESS)
	{
		w->overlayPass = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

bool vk_writeout_init(vk_writeout *w, const VkImageView *views, u32 count, u32 width, u32 height)
{
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo poolInfo;
	VkDescriptorSetAllocateInfo setInfo;
	VkDescriptorSetLayout *layouts;
	VkResult res;
	u32 i;

	drop_sized(w);
	if (w->setLayout == VK_NULL_HANDLE && !make_layouts(w))
		return false;
	if (!vk_writeout_wanted() || count == 0)
		return true;

	sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[0].descriptorCount = count;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = count;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = count;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(vkc.device, &poolInfo, NULL, &w->pool) != VK_SUCCESS)
	{
		w->pool = VK_NULL_HANDLE;
		return false;
	}
	w->sets = (VkDescriptorSet *)calloc(count, sizeof(VkDescriptorSet));
	w->overlayFramebuffers = (VkFramebuffer *)calloc(count, sizeof(VkFramebuffer));
	layouts = (VkDescriptorSetLayout *)malloc(count * sizeof(VkDescriptorSetLayout));
	if (w->sets == NULL || w->overlayFramebuffers == NULL || layouts == NULL)
	{
		free(layouts);
		drop_sized(w);
		return false;
	}
	for (i = 0; i < count; i++)
		layouts[i] = w->setLayout;
	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = w->pool;
	setInfo.descriptorSetCount = count;
	setInfo.pSetLayouts = layouts;
	res = vkAllocateDescriptorSets(vkc.device, &setInfo, w->sets);
	free(layouts);
	w->count = count;
	if (res != VK_SUCCESS
			|| vk_buffer_init(&w->words, (VkDeviceSize)SLOT_BYTES * count, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
	{
		drop_sized(w);
		return false;
	}
	w->mapped = (u32 *)vk_buffer_map(&w->words);
	for (i = 0; i < count; i++)
	{
		VkDescriptorBufferInfo info;
		VkWriteDescriptorSet write;
		info.buffer = w->words.buffer;
		info.offset = (VkDeviceSize)SLOT_BYTES * i;
		info.range = SLOT_BYTES;
		memset(&write, 0, sizeof(write));
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = w->sets[i];
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo = &info;
		vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);
	}

	if (!make_overlay_pass(w))
	{
		drop_sized(w);
		return false;
	}
	w->extent.width = width;
	w->extent.height = height;
	for (i = 0; i < count; i++)
	{
		VkFramebufferCreateInfo info;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = w->overlayPass;
		info.attachmentCount = 1;
		info.pAttachments = &views[i];
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (vkCreateFramebuffer(vkc.device, &info, NULL, &w->overlayFramebuffers[i]) != VK_SUCCESS)
		{
			w->overlayFramebuffers[i] = VK_NULL_HANDLE;
			drop_sized(w);
			return false;
		}
	}
	return true;
}

void vk_writeout_begin_overlay(vk_writeout *w, VkCommandBuffer cmd, u32 index)
{
	VkRenderPassBeginInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = w->overlayPass;
	info.framebuffer = w->overlayFramebuffers[index];
	info.renderArea.extent = w->extent;
	vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
		VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
	VkImageMemoryBarrier b;
	memset(&b, 0, sizeof(b));
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

void vk_writeout_record(vk_writeout *w, VkCommandBuffer cmd, u32 index, VkImage image, VkImageView view, u32 width, u32 height)
{
	VkDescriptorImageInfo info;
	VkWriteDescriptorSet write;
	u32 flags, *words;
	/* (the pipeline thread's, made before the first frame; one it could not
	 * make leaves the picture as the render pass drew it) */
	const VkPipeline pipeline = vk_pso_get(&w->slot);
	if (pipeline == VK_NULL_HANDLE || index >= w->count)
		return;
	flags = compute_frame_write_out(height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	/* (this picture's set and slot: the frame before that drew in it is
	 * done, as the command pool has waited for it) */
	words = w->mapped + index * (SLOT_BYTES / 4);
	words[PVRC_F_WIDTH] = width;
	words[PVRC_F_HEIGHT] = height;
	words[PVRC_F_FLAGS] = flags;
	info.sampler = VK_NULL_HANDLE;
	info.imageView = view;
	info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = w->sets[index];
	write.dstBinding = 3;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);

	barrier(cmd, image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, w->pipelineLayout, 0, 1, &w->sets[index], 0, NULL);
	vkCmdDispatch(cmd, (width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	/* (for the frontend and the next frame's drawing over it, and a copy when the context goes) */
	barrier(cmd, image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
			VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
}
