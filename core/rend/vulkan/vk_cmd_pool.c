/* See vk_cmd_pool.h. */
#include <stdlib.h>
#include <string.h>
#include "vk_cmd_pool.h"

static void destroy_deferred(vk_cmd_slot *slot)
{
	const vk_cmd_deferred *d = (const vk_cmd_deferred *)slot->deferred.data;
	u32 i;
	for (i = 0; i < slot->deferredCount; i++)
	{
		if (d[i].destroy != NULL)
			d[i].destroy(d[i].object);
		else
			vkFreeDescriptorSets(vkc.device, d[i].setPool, 1, &d[i].set);
	}
	slot->deferredCount = 0;
}

/* Slot @i taken down: what it deferred, its pool (and the buffers made
 * from it, with it), its fence */
static void free_slot(vk_cmd_pool *p, u32 i)
{
	vk_cmd_slot *slot = &p->slots[i];
	destroy_deferred(slot);
	grow_buf_free(&slot->deferred);
	grow_buf_free(&slot->buffers);
	if (slot->pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(vkc.device, slot->pool, NULL);
	if (p->fences[i] != VK_NULL_HANDLE)
		vkDestroyFence(vkc.device, p->fences[i], NULL);
	memset(slot, 0, sizeof(*slot));
	p->fences[i] = VK_NULL_HANDLE;
}

bool vk_cmd_pool_init(vk_cmd_pool *p)
{
	const u32 size = (u32)vkc.swapChainSize;
	u32 i;
	bool made = true;

	/* (the places gone: their pools, and the buffers made from them, with them) */
	for (i = size; i < p->slotCount; i++)
		free_slot(p, i);
	if (size > p->slotCount)
	{
		vk_cmd_slot *grownSlots = (vk_cmd_slot *)realloc(p->slots, size * sizeof(vk_cmd_slot));
		VkFence *grownFences;
		if (grownSlots != NULL)
			p->slots = grownSlots;
		grownFences = (VkFence *)realloc(p->fences, size * sizeof(VkFence));
		if (grownFences != NULL)
			p->fences = grownFences;
		if (grownSlots == NULL || grownFences == NULL)
			return false;
		for (i = p->slotCount; i < size; i++)
		{
			VkCommandPoolCreateInfo poolInfo;
			VkFenceCreateInfo fenceInfo;
			memset(&p->slots[i], 0, sizeof(p->slots[i]));
			p->fences[i] = VK_NULL_HANDLE;
			memset(&poolInfo, 0, sizeof(poolInfo));
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			poolInfo.queueFamilyIndex = vkc.iface->queue_index;
			if (vkCreateCommandPool(vkc.device, &poolInfo, NULL, &p->slots[i].pool) != VK_SUCCESS)
			{
				p->slots[i].pool = VK_NULL_HANDLE;
				made = false;
			}
			memset(&fenceInfo, 0, sizeof(fenceInfo));
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			if (vkCreateFence(vkc.device, &fenceInfo, NULL, &p->fences[i]) != VK_SUCCESS)
			{
				p->fences[i] = VK_NULL_HANDLE;
				made = false;
			}
		}
	}
	p->slotCount = size;
	if (p->index >= (int)size)
		p->index = 0;
	return made;
}

void vk_cmd_pool_term(vk_cmd_pool *p)
{
	u32 i;
	/* what was deferred first, while what it may need is still there */
	for (i = 0; i < p->slotCount; i++)
		destroy_deferred(&p->slots[i]);
	for (i = 0; i < p->slotCount; i++)
		free_slot(p, i);
	free(p->slots);
	free(p->fences);
	p->slots = NULL;
	p->fences = NULL;
	p->slotCount = 0;
}

void vk_cmd_pool_end_frame(vk_cmd_pool *p)
{
	const vk_cmd_slot *slot = &p->slots[p->index];
	vkc_submit(slot->used, (const VkCommandBuffer *)slot->buffers.data, p->fences[p->index]);
}

void vk_cmd_pool_begin_frame(vk_cmd_pool *p)
{
	vk_cmd_slot *slot;
	p->frames++;
	p->index = (p->index + 1) % vkc.swapChainSize;
	vkWaitForFences(vkc.device, 1, &p->fences[p->index], VK_TRUE, UINT64_MAX);
	vkResetFences(vkc.device, 1, &p->fences[p->index]);
	/* The fence above has signaled, so the GPU is done with everything that
	 * was submitted for this slot: anything retired against it is safe to
	 * destroy now. */
	slot = &p->slots[p->index];
	destroy_deferred(slot);
	slot->used = 0;
	/* (the memory the commands were recorded in stays the pool's, for the
	 * next frame's: given back to the driver, it was taken again) */
	vkResetCommandPool(vkc.device, slot->pool, 0);
}

VkCommandBuffer vk_cmd_pool_allocate(vk_cmd_pool *p)
{
	vk_cmd_slot *slot = &p->slots[p->index];
	if (slot->used == slot->made)
	{
		VkCommandBufferAllocateInfo info;
		VkCommandBuffer *list = (VkCommandBuffer *)grow_buf_reserve_keep(&slot->buffers,
				(slot->made + 1) * sizeof(VkCommandBuffer));
		if (list == NULL)
			return VK_NULL_HANDLE;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		info.commandPool = slot->pool;
		info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		info.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(vkc.device, &info, &list[slot->made]) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		slot->made++;
	}
	return ((const VkCommandBuffer *)slot->buffers.data)[slot->used++];
}

void vk_cmd_pool_wait_all(vk_cmd_pool *p)
{
	if (p->slotCount == 0)
		return;
	if (vkWaitForFences(vkc.device, p->slotCount, p->fences, VK_TRUE, 1000000000ull) != VK_SUCCESS)
		vkc_wait_queue_idle();
}

bool vk_cmd_pool_is_done(const vk_cmd_pool *p, int slot)
{
	return vkGetFenceStatus(vkc.device, p->fences[slot]) == VK_SUCCESS;
}

/* room for one more deferred, or NULL: the frames that may use it waited for */
static vk_cmd_deferred *defer_room(vk_cmd_pool *p)
{
	vk_cmd_slot *slot = &p->slots[p->index];
	vk_cmd_deferred *list = (vk_cmd_deferred *)grow_buf_reserve_keep(&slot->deferred,
			(slot->deferredCount + 1) * sizeof(vk_cmd_deferred));
	if (list == NULL)
	{
		vkc_wait_queue_idle();
		return NULL;
	}
	return &list[slot->deferredCount++];
}

void vk_cmd_pool_defer(vk_cmd_pool *p, void *object, void (*destroy)(void *object))
{
	vk_cmd_deferred *d;
	if (object == NULL)
		return;
	if (p->slotCount == 0)
	{
		/* (as a unique pointer let go of) */
		destroy(object);
		return;
	}
	d = defer_room(p);
	if (d == NULL)
	{
		destroy(object);
		return;
	}
	d->object = object;
	d->destroy = destroy;
}

void vk_cmd_pool_defer_set(vk_cmd_pool *p, VkDescriptorSet set, VkDescriptorPool pool)
{
	vk_cmd_deferred *d;
	if (set == VK_NULL_HANDLE)
		return;
	d = p->slotCount != 0 ? defer_room(p) : NULL;
	if (d == NULL)
	{
		vkFreeDescriptorSets(vkc.device, pool, 1, &set);
		return;
	}
	d->object = NULL;
	d->destroy = NULL;
	d->set = set;
	d->setPool = pool;
}
