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
#include "vk_cmd_pool.h"
#include <memory>

/* The C++ renderers' view of a vk_cmd_pool, until they are C too */
class CommandPool
{
	template<typename T>
	static void DeleteObject(void *object)
	{
		delete (T *)object;
	}

public:
	CommandPool() { memset(&p, 0, sizeof(p)); }
	CommandPool(const CommandPool&) = delete;
	CommandPool& operator=(const CommandPool&) = delete;
	~CommandPool() { Term(); }

	void Init()
	{
		if (!vk_cmd_pool_init(&p))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_cmd_pool_init");
	}
	void Term() { vk_cmd_pool_term(&p); }
	void EndFrame() { vk_cmd_pool_end_frame(&p); }
	void BeginFrame() { vk_cmd_pool_begin_frame(&p); }
	vk::CommandBuffer Allocate() { return vk::CommandBuffer(vk_cmd_pool_allocate(&p)); }
	void WaitAll() { vk_cmd_pool_wait_all(&p); }
	vk::Fence GetCurrentFence() { return vk::Fence(vk_cmd_pool_fence(&p)); }
	bool IsDone(int slot) const { return vk_cmd_pool_is_done(&p, slot); }
	int GetSize() const { return (int)p.slotCount; }
	int GetIndex() const { return p.index; }
	/* how many frames have begun: the one being recorded is the last of them */
	u64 GetFrames() const { return p.frames; }

	template<typename T>
	void DeferDelete(std::unique_ptr<T> obj)
	{
		if (obj)
			vk_cmd_pool_defer(&p, obj.release(), DeleteObject<T>);
	}
	void DeferFree(vk::UniqueDescriptorSet set, vk::DescriptorPool pool)
	{
		if (set)
			vk_cmd_pool_defer_set(&p, (VkDescriptorSet)set.release(), (VkDescriptorPool)pool);
	}

	vk_cmd_pool p;
};
