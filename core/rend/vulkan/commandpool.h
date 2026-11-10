/*
	Created on: Oct 8, 2019

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
#pragma once
#include "vulkan_context.h"
#include "rend/grow_buf.h"
#include <memory>

class CommandPool
{
	/* A frame's place in the ring: its command buffers, made from its pool
	 * as they are first asked for and used again every time round (the
	 * pool reset frees what they recorded, not them; destroying the pool
	 * frees them), and what is to be destroyed once its fence has
	 * signaled. Memory only ever grown (grow_buf.h). */
	struct Deferred
	{
		void *object;
		void (*destroy)(void *object);	/* none: a descriptor set, given back to its pool */
		vk::DescriptorSet set;
		vk::DescriptorPool setPool;
	};
	struct Slot
	{
		vk::CommandPool pool;
		grow_buf_t buffers = GROW_BUF_INIT;	/* vk::CommandBuffer */
		u32 made = 0, used = 0;
		grow_buf_t deferred = GROW_BUF_INIT;	/* Deferred */
		u32 deferredCount = 0;
	};

	template<typename T>
	static void DeleteObject(void *object)
	{
		delete (T *)object;
	}

	Slot *slots = nullptr;
	vk::Fence *fences = nullptr;	/* a slot's each, side by side for WaitAll() */
	u32 slotCount = 0;

	static void DestroyDeferred(Slot& slot)
	{
		const Deferred *d = (const Deferred *)slot.deferred.data;
		for (u32 i = 0; i < slot.deferredCount; i++)
		{
			if (d[i].destroy != nullptr)
				d[i].destroy(d[i].object);
			else
				VulkanContext::Instance()->GetDevice().freeDescriptorSets(d[i].setPool, 1, &d[i].set);
		}
		slot.deferredCount = 0;
	}

	/* Slot @i taken down: what it deferred, its pool (and the buffers made
	 * from it, with it), its fence */
	void FreeSlot(u32 i)
	{
		const vk::Device device = VulkanContext::Instance()->GetDevice();
		Slot& slot = slots[i];
		DestroyDeferred(slot);
		grow_buf_free(&slot.deferred);
		grow_buf_free(&slot.buffers);
		if (slot.pool)
			device.destroyCommandPool(slot.pool);
		if (fences[i])
			device.destroyFence(fences[i]);
		slot = Slot();
		fences[i] = vk::Fence();
	}

public:
	CommandPool() = default;
	CommandPool(const CommandPool&) = delete;
	CommandPool& operator=(const CommandPool&) = delete;
	~CommandPool()
	{
		Term();
	}

	void Init()
	{
		const u32 size = (u32)VulkanContext::Instance()->GetSwapChainSize();
		const vk::Device device = VulkanContext::Instance()->GetDevice();

		/* (the places gone: their pools, and the buffers made from them, with them) */
		for (u32 i = size; i < slotCount; i++)
			FreeSlot(i);
		if (size > slotCount)
		{
			Slot *grownSlots = (Slot *)realloc(slots, size * sizeof(Slot));
			if (grownSlots != nullptr)
				slots = grownSlots;
			vk::Fence *grownFences = (vk::Fence *)realloc(fences, size * sizeof(vk::Fence));
			if (grownFences != nullptr)
				fences = grownFences;
			if (grownSlots == nullptr || grownFences == nullptr)
				return;
			for (u32 i = slotCount; i < size; i++)
			{
				slots[i] = Slot();
				slots[i].pool = device.createCommandPool(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient,
						VulkanContext::Instance()->GetGraphicsQueueFamilyIndex()));
				fences[i] = device.createFence(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
			}
		}
		slotCount = size;
		if (index >= (int)size)
			index = 0;
	}

	void Term()
	{
		/* what was deferred first, while what it may need is still there */
		for (u32 i = 0; i < slotCount; i++)
			DestroyDeferred(slots[i]);
		for (u32 i = 0; i < slotCount; i++)
			FreeSlot(i);
		free(slots);
		free(fences);
		slots = nullptr;
		fences = nullptr;
		slotCount = 0;
	}

	void EndFrame()
	{
		/* (the buffers, as they are: nothing copied or allocated a submit) */
		const Slot& slot = slots[index];
		VulkanContext::Instance()->SubmitCommandBuffers(slot.used, (vk::CommandBuffer *)slot.buffers.data, fences[index]);
	}

	void BeginFrame()
	{
		frames++;
		index = (index + 1) % VulkanContext::Instance()->GetSwapChainSize();
		VulkanContext::Instance()->GetDevice().waitForFences(1, &fences[index], true, UINT64_MAX);
		VulkanContext::Instance()->GetDevice().resetFences(1, &fences[index]);
		/* The fence above has signaled, so the GPU is done with everything that was
		   submitted for this slot. Anything retired against it is now safe to destroy. */
		Slot& slot = slots[index];
		DestroyDeferred(slot);
		slot.used = 0;
		/* (the memory the commands were recorded in stays the pool's, for the
		 * next frame's: given back to the driver, it was taken again) */
		VulkanContext::Instance()->GetDevice().resetCommandPool(slot.pool, vk::CommandPoolResetFlags());
	}

	vk::CommandBuffer Allocate()
	{
		Slot& slot = slots[index];
		if (slot.used == slot.made)
		{
			vk::CommandBuffer *list = (vk::CommandBuffer *)grow_buf_reserve_keep(&slot.buffers,
					(slot.made + 1) * sizeof(vk::CommandBuffer));
			if (list == nullptr)
				return vk::CommandBuffer();
			const vk::CommandBufferAllocateInfo info(slot.pool, vk::CommandBufferLevel::ePrimary, 1);
			if (VulkanContext::Instance()->GetDevice().allocateCommandBuffers(&info, &list[slot.made]) != vk::Result::eSuccess)
				return vk::CommandBuffer();
			slot.made++;
		}
		return ((const vk::CommandBuffer *)slot.buffers.data)[slot.used++];
	}

	/* Waits until the GPU is done with everything submitted through this
	 * pool. For before things those frames use are destroyed outright: when
	 * the renderer is taken down or resized. The frontend's own wait
	 * (wait_sync_index) is for the frontend's frames, not these.
	 *
	 * It gives up on the fences after a second rather than hang on a frame
	 * that was begun and never submitted, and waits for the queue instead:
	 * a frame that takes longer than that on the GPU is still being drawn,
	 * and what it uses is not to be destroyed under it. */
	void WaitAll()
	{
		if (slotCount == 0)
			return;
		if (VulkanContext::Instance()->GetDevice().waitForFences(slotCount, fences, true, 1000000000ull) != vk::Result::eSuccess)
			VulkanContext::Instance()->WaitQueueIdle();
	}

	vk::Fence GetCurrentFence()
	{
		return fences[index];
	}

	/* Whether the GPU is done with what was last submitted for @slot:
	 * its fence, polled */
	bool IsDone(int slot) const
	{
		return VulkanContext::Instance()->GetDevice().getFenceStatus(fences[slot]) == vk::Result::eSuccess;
	}

	int GetSize() const
	{
		return (int)slotCount;
	}

	int GetIndex() const
	{
		return index;
	}

	// how many frames have begun: the one being recorded is the last of them
	u64 GetFrames() const
	{
		return frames;
	}

	/* Retire a GPU resource so it is destroyed once the current frame's fence has
	   signaled, rather than stalling the whole device with WaitIdle. Used when an
	   attachment must be recreated (grown) while previous frames may still reference
	   the old one. The retired object is freed in BeginFrame when this slot recurs. */
	template<typename T>
	void DeferDelete(std::unique_ptr<T> obj)
	{
		if (!obj || slotCount == 0)
			return;
		Slot& slot = slots[index];
		Deferred *list = (Deferred *)grow_buf_reserve_keep(&slot.deferred,
				(slot.deferredCount + 1) * sizeof(Deferred));
		if (list == nullptr)
		{
			/* (no memory to defer it in: the frames that may use it waited for, then destroyed) */
			VulkanContext::Instance()->WaitQueueIdle();
			return;
		}
		list[slot.deferredCount].object = obj.release();
		list[slot.deferredCount].destroy = DeleteObject<T>;
		slot.deferredCount++;
	}

	/* A descriptor set retired the same way, given back to @pool: nothing
	 * allocated to hold it (the list keeps its room) */
	void DeferFree(vk::UniqueDescriptorSet set, vk::DescriptorPool pool)
	{
		if (!set || slotCount == 0)
			return;
		Slot& slot = slots[index];
		Deferred *list = (Deferred *)grow_buf_reserve_keep(&slot.deferred,
				(slot.deferredCount + 1) * sizeof(Deferred));
		if (list == nullptr)
		{
			/* (no room to defer it in: the frames that may use it waited for, then freed) */
			VulkanContext::Instance()->WaitQueueIdle();
			return;
		}
		list[slot.deferredCount].object = nullptr;
		list[slot.deferredCount].destroy = nullptr;
		list[slot.deferredCount].set = set.release();
		list[slot.deferredCount].setPool = pool;
		slot.deferredCount++;
	}

private:
	int index = 0;
	u64 frames = 0;
};
