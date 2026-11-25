/*
    Created on: Nov 12, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <string.h>
#include "vk_oit_buffer.h"

void vk_oit_buffers_construct(vk_oit_buffers *o)
{
	memset(o, 0, sizeof(*o));
	o->countSlot = VK_OIT_COUNT_SLOTS;
	oit_bands_init(&o->bandState);
}

static VkDeviceSize binding_limit(void)
{
	const VkDeviceSize range = vkc.maxStorageBufferRange;
	return (vkc.maxMemoryAllocationSize < range ? vkc.maxMemoryAllocationSize : range) & ~(VkDeviceSize)(OIT_FRAGMENT_BYTES - 1);
}

VkDeviceSize vk_oit_buffers_planned_size(const vk_oit_buffers *o, int width, int height)
{
	const u32 kib = vram_budget_available_kib(&vkc.vramBudget);
	const u32 w = width > 0 ? (u32)width : 0;
	const u32 h = height > 0 ? (u32)height : 0;
	if (o->compact)
		return oit_fallback_buffer_bytes(w, h, kib, (u64)binding_limit() * VK_OIT_BINDINGS);
	return oit_buffer_bytes(w, h, kib, (u64)binding_limit() * VK_OIT_BINDINGS);
}

/* The fragment buffer, of plannedSize: in as few storage buffers as the
 * device allows, all of one size. The ones it replaces are retired like
 * the pointer image, and so is the counter's reset value, which says how
 * many fragments there is room for. */
static bool make_pixel_buffers(vk_oit_buffers *o, vk_cmd_pool *pool)
{
	const VkDeviceSize limit = binding_limit();
	const VkDeviceSize parts = (o->plannedSize + limit - 1) / limit;
	VkDeviceSize capacity;
	u32 reset[2];
	u32 i;
	o->bindings = (u32)(parts < VK_OIT_BINDINGS ? parts : VK_OIT_BINDINGS);
	o->bindingSize = (o->plannedSize / o->bindings) & ~(VkDeviceSize)(OIT_FRAGMENT_BYTES - 1);
	o->bufferSize = o->plannedSize;
	capacity = o->bindingSize / OIT_FRAGMENT_BYTES * o->bindings;
	o->capacity = (u32)(capacity < 0xffffffffu ? capacity : 0xffffffffu);
	for (i = 0; i < VK_OIT_BINDINGS; i++)
	{
		vk_buffer_defer(pool, &o->pixelBuffers[i]);
		if (i < o->bindings && vk_buffer_init(&o->pixelBuffers[i], o->bindingSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != VK_SUCCESS)
			return false;
	}
	if (o->pixelCounter.buffer == VK_NULL_HANDLE && vk_buffer_init(&o->pixelCounter, 2 * sizeof(u32),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != VK_SUCCESS)
		return false;
	vk_buffer_defer(pool, &o->pixelCounterReset);
	if (vk_buffer_init(&o->pixelCounterReset, 2 * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return false;
	reset[0] = 0;
	reset[1] = o->capacity;
	vk_buffer_upload(&o->pixelCounterReset, sizeof(reset), reset, 0);
	NOTICE_LOG(RENDERER, "Vulkan per-pixel renderer: fragment buffer of %u MB in %u storage buffer%s (room for %u fragments)\n",
			(u32)(o->bufferSize >> 20), o->bindings, o->bindings > 1 ? "s" : "", o->capacity);
	return true;
}

bool vk_oit_buffers_init(vk_oit_buffers *o, int width, int height, vk_cmd_pool *pool)
{
	bool remake = false;
	VkWriteDescriptorSet writes[4];
	VkDescriptorBufferInfo pixelBufferInfo[VK_OIT_BINDINGS];
	VkDescriptorBufferInfo pixelCounterBufferInfo;
	VkDescriptorImageInfo pointerImageInfo, sumsImageInfo;
	VkDescriptorSetAllocateInfo alloc;
	u32 count = 0;
	u32 i;
	if (o->descSetLayout == VK_NULL_HANDLE)
	{
		/* the pixel buffers, the pixel counter, the a-buffer pointers and
		 * (SSAA's channel sums, for the HDR output: written only with
		 * sums, read by its shader alone) */
		VkDescriptorSetLayoutBinding bindings[4];
		VkDescriptorSetLayoutCreateInfo info;
		memset(bindings, 0, sizeof(bindings));
		for (i = 0; i < 4; i++)
		{
			bindings[i].binding = i;
			bindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[i].descriptorCount = i == 0 ? VK_OIT_BINDINGS : 1;
			bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = 4;
		info.pBindings = bindings;
		if (vkCreateDescriptorSetLayout(vkc.device, &info, NULL, &o->descSetLayout) != VK_SUCCESS)
			return false;
	}
	/* (the renderer's set-up: the layout only, the buffers are for the screen's size) */
	if (width <= 0 || height <= 0)
		return true;
	if (o->plannedSize == 0)
		o->plannedSize = vk_oit_buffers_planned_size(o, width, height);
	if (o->pixelBuffers[0].buffer == VK_NULL_HANDLE || o->plannedSize != o->bufferSize)
	{
		if (!make_pixel_buffers(o, pool))
			return false;
		remake = true;
	}
	if (o->counts.buffer == VK_NULL_HANDLE)
	{
		/* (read by the CPU once a frame's fence has passed: see vk_oit_buffers_choose_bands()) */
		if (vk_buffer_init(&o->counts, VK_OIT_COUNT_SLOTS * VK_OIT_COUNTS * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
			return false;
		memset(o->written, 0, sizeof(o->written));
	}
	if (width > o->maxWidth || height > o->maxHeight || o->abufferPointerAttachment.image == VK_NULL_HANDLE
			|| o->sums != (o->sumsAttachment.image != VK_NULL_HANDLE))
	{
		if (width > o->maxWidth)
			o->maxWidth = width;
		if (height > o->maxHeight)
			o->maxHeight = height;
		/* Frames already handed to the GPU may still be using the image
		 * and the descriptor set that names it. Neither is touched: the
		 * image is retired, to be destroyed once those frames are done,
		 * and so is the descriptor set, with a new one written in its
		 * place below. */
		vk_attachment_defer(pool, &o->abufferPointerAttachment);
		/* (cleared when it is first used: see vk_oit_buffers_on_new_frame()) */
		if (!vk_attachment_init(&o->abufferPointerAttachment, (u32)o->maxWidth, (u32)o->maxHeight, VK_FORMAT_R32_UINT,
				VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, VK_SAMPLE_COUNT_1_BIT))
			return false;
		o->abufferPointerTransitionNeeded = true;
		vk_attachment_defer(pool, &o->sumsAttachment);
		if (o->sums && !vk_attachment_init(&o->sumsAttachment, (u32)o->maxWidth, (u32)o->maxHeight, VK_FORMAT_R16G16B16A16_UINT,
				VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, VK_SAMPLE_COUNT_1_BIT))
			return false;
		o->firstFrameAfterInit = true;
		remake = true;
	}
	if (!remake)
		return true;

	if (o->descSet != VK_NULL_HANDLE)
		vk_cmd_pool_defer_set(pool, o->descSet, vkc.descriptorPool);
	o->descSet = VK_NULL_HANDLE;
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = vkc.descriptorPool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &o->descSetLayout;
	if (vkAllocateDescriptorSets(vkc.device, &alloc, &o->descSet) != VK_SUCCESS)
	{
		o->descSet = VK_NULL_HANDLE;
		return false;
	}
	/* (on the stack: nothing allocated for them; the buffers not needed
	 * are given the first again: the shaders never go to them) */
	memset(writes, 0, sizeof(writes));
	for (i = 0; i < VK_OIT_BINDINGS; i++)
	{
		pixelBufferInfo[i].buffer = o->pixelBuffers[i < o->bindings ? i : 0].buffer;
		pixelBufferInfo[i].offset = 0;
		pixelBufferInfo[i].range = o->bindingSize;
	}
	pixelCounterBufferInfo.buffer = o->pixelCounter.buffer;
	pixelCounterBufferInfo.offset = 0;
	pixelCounterBufferInfo.range = 2 * sizeof(u32);
	pointerImageInfo.sampler = VK_NULL_HANDLE;
	pointerImageInfo.imageView = o->abufferPointerAttachment.view;
	pointerImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	sumsImageInfo.sampler = VK_NULL_HANDLE;
	sumsImageInfo.imageView = o->sumsAttachment.view;
	sumsImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	for (i = 0; i < 4; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = o->descSet;
		writes[i].dstBinding = i;
		writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].descriptorCount = 1;
	}
	writes[0].descriptorCount = VK_OIT_BINDINGS;
	writes[0].pBufferInfo = pixelBufferInfo;
	writes[1].pBufferInfo = &pixelCounterBufferInfo;
	writes[2].pImageInfo = &pointerImageInfo;
	writes[3].pImageInfo = &sumsImageInfo;
	count = o->sumsAttachment.image != VK_NULL_HANDLE ? 4 : 3;
	vkUpdateDescriptorSets(vkc.device, count, writes, 0, NULL);
	return true;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages)
{
	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = src;
	barrier.dstAccessMask = dst;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, srcStages, dstStages, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void buffer_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkAccessFlags src,
		VkAccessFlags dst, VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages)
{
	VkBufferMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = src;
	barrier.dstAccessMask = dst;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer;
	barrier.offset = offset;
	barrier.size = size;
	vkCmdPipelineBarrier(cmd, srcStages, dstStages, 0, 0, NULL, 1, &barrier, 0, NULL);
}

void vk_oit_buffers_on_new_frame(vk_oit_buffers *o, VkCommandBuffer cmd)
{
	if (o->sumsAttachment.image != VK_NULL_HANDLE)
		/* The sums: in the general layout from the first; after that, the
		 * frame before's encode done reading them before this frame's last
		 * pass stores them again */
		image_barrier(cmd, o->sumsAttachment.image, 0, VK_ACCESS_SHADER_WRITE_BIT,
				o->abufferPointerTransitionNeeded ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	if (o->abufferPointerTransitionNeeded)
	{
		VkClearColorValue endOfList;
		VkImageSubresourceRange range;
		o->abufferPointerTransitionNeeded = false;
		/* (made ready for the clear below, which is what uses it next) */
		image_barrier(cmd, o->abufferPointerAttachment.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		/* The lists start empty. The image is new and holds nothing that
		 * means anything; every frame leaves the lists empty for the next,
		 * and this does it for the first. */
		endOfList.uint32[0] = endOfList.uint32[1] = endOfList.uint32[2] = endOfList.uint32[3] = 0xffffffffu;
		memset(&range, 0, sizeof(range));
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;
		vkCmdClearColorImage(cmd, o->abufferPointerAttachment.image, VK_IMAGE_LAYOUT_GENERAL, &endOfList, 1, &range);
		image_barrier(cmd, o->abufferPointerAttachment.image, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	else
		o->firstFrameAfterInit = false;
}

void vk_oit_buffers_reset_pixel_counter(vk_oit_buffers *o, VkCommandBuffer cmd)
{
	VkBufferCopy copy;
	/* The render passes and frames before this must be done counting... */
	buffer_barrier(cmd, o->pixelCounter.buffer, 0, VK_WHOLE_SIZE, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	/* (the count back at nothing, and after it how many fragments there is room for) */
	copy.srcOffset = 0;
	copy.dstOffset = 0;
	copy.size = 2 * sizeof(u32);
	vkCmdCopyBuffer(cmd, o->pixelCounterReset.buffer, o->pixelCounter.buffer, 1, &copy);
	/* ...and the next render pass's shaders must see it back at nothing. (From upstream.) */
	buffer_barrier(cmd, o->pixelCounter.buffer, 0, VK_WHOLE_SIZE, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void vk_oit_buffers_count_pass(vk_oit_buffers *o, VkCommandBuffer cmd)
{
	VkDeviceSize offset;
	VkBufferCopy copy;
	if (o->countSlot >= VK_OIT_COUNT_SLOTS || o->written[o->countSlot] >= VK_OIT_COUNTS)
		return;
	buffer_barrier(cmd, o->pixelCounter.buffer, 0, VK_WHOLE_SIZE, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	offset = (VkDeviceSize)(o->countSlot * VK_OIT_COUNTS + o->written[o->countSlot]) * sizeof(u32);
	copy.srcOffset = 0;
	copy.dstOffset = offset;
	copy.size = sizeof(u32);
	vkCmdCopyBuffer(cmd, o->pixelCounter.buffer, o->counts.buffer, 1, &copy);
	/* (and the counter is not reset before it has been read) */
	buffer_barrier(cmd, o->counts.buffer, offset, sizeof(u32), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT);
	o->written[o->countSlot]++;
}

u32 vk_oit_buffers_choose_bands(vk_oit_buffers *o, int slot, bool rtt)
{
	o->countSlot = slot >= 0 && slot < (int)VK_OIT_COUNT_SLOTS ? (u32)slot : VK_OIT_COUNT_SLOTS;
	if (o->countSlot == VK_OIT_COUNT_SLOTS)
		return o->bandState.bands[rtt ? 1 : 0];
	if (o->written[o->countSlot] != 0)
	{
		const u32 *count = (const u32 *)vk_buffer_map(&o->counts) + o->countSlot * VK_OIT_COUNTS;
		const u32 kind = o->bandState.rtt[o->countSlot];
		u32 fragments = 0;
		u32 i;
		for (i = 0; i < o->written[o->countSlot]; i++)
			if (count[i] > fragments)
				fragments = count[i];
		if (oit_bands_count(&o->bandState, o->countSlot, fragments, o->capacity))
			NOTICE_LOG(RENDERER, "Vulkan per-pixel renderer: %u translucent fragments in a band of %u%s, room for %u: %u band%s\n",
					fragments, o->bandState.drawn_in[o->countSlot] > 1 ? o->bandState.drawn_in[o->countSlot] : 1u,
					kind ? " (a render to a texture)" : "", o->capacity, o->bandState.bands[kind],
					o->bandState.bands[kind] > 1 ? "s" : "");
	}
	o->written[o->countSlot] = 0;
	return oit_bands_begin(&o->bandState, o->countSlot, rtt);
}

void vk_oit_buffers_term(vk_oit_buffers *o)
{
	u32 i;
	for (i = 0; i < VK_OIT_BINDINGS; i++)
		vk_buffer_free(&o->pixelBuffers[i]);
	memset(o->pixelBuffers, 0, sizeof(o->pixelBuffers));
	o->bufferSize = 0;
	vk_buffer_free(&o->pixelCounter);
	vk_buffer_free(&o->pixelCounterReset);
	vk_buffer_free(&o->counts);
	memset(&o->pixelCounter, 0, sizeof(vk_buffer));
	memset(&o->pixelCounterReset, 0, sizeof(vk_buffer));
	memset(&o->counts, 0, sizeof(vk_buffer));
	vk_attachment_free(&o->abufferPointerAttachment);
	vk_attachment_free(&o->sumsAttachment);
	memset(&o->abufferPointerAttachment, 0, sizeof(vk_attachment));
	memset(&o->sumsAttachment, 0, sizeof(vk_attachment));
	if (o->descSet != VK_NULL_HANDLE)
		vkFreeDescriptorSets(vkc.device, vkc.descriptorPool, 1, &o->descSet);
	o->descSet = VK_NULL_HANDLE;
	o->maxWidth = o->maxHeight = 0;
	oit_bands_init(&o->bandState);
}

void vk_oit_buffers_free(vk_oit_buffers *o)
{
	vk_oit_buffers_term(o);
	if (o->descSetLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, o->descSetLayout, NULL);
	o->descSetLayout = VK_NULL_HANDLE;
}
