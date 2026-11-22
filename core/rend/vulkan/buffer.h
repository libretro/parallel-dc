/*
 *  Created on: Oct 3, 2019

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
#include "vulkan.h"
#include "vk_buffer.h"
#include "heap.h"

/* The C++ renderers' view of a vk_buffer, until they are C too */
struct BufferData
{
	/* the buffer, as vk::UniqueBuffer was read */
	struct Handle
	{
		vk::Buffer h;
		const vk::Buffer& operator*() const { return h; }
		const vk::Buffer& get() const { return h; }
		explicit operator bool() const { return (bool)h; }
	};
	/* its memory, as Allocation was */
	struct Memory
	{
		const vk_buffer *b;
		void *MapMemory() const { return vk_buffer_map(b); }
		VkMemoryPropertyFlags PropertyFlags() const { return vk_buffer_memory_flags(b); }
	};

	BufferData(vk::DeviceSize size, const vk::BufferUsageFlags& usage,
			const vk::MemoryPropertyFlags& propertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
	{
		const VkResult res = vk_buffer_init(&b, size, (VkBufferUsageFlags)usage, (VkMemoryPropertyFlags)propertyFlags);
		if (res != VK_SUCCESS)
			throwResultException((vk::Result)res, "vk_buffer_init");
		buffer.h = vk::Buffer(b.buffer);
		bufferSize = size;
		allocation.b = &b;
	}
	~BufferData() { vk_buffer_free(&b); }
	BufferData(const BufferData&) = delete;
	BufferData& operator=(const BufferData&) = delete;

	void upload(u32 size, const void *data, u32 bufOffset = 0) const { vk_buffer_upload(&b, size, data, bufOffset); }
	void upload(size_t count, u32 *sizes, const void **data, u32 bufOffset = 0) const
	{
		vk_buffer_upload_many(&b, count, sizes, data, bufOffset);
	}
	void download(u32 size, void *data, u32 bufOffset = 0) const { vk_buffer_download(&b, size, data, bufOffset); }
	void *MapMemory() { return vk_buffer_map(&b); }
	void UnmapMemory() {}

	vk_buffer b;
	Handle buffer;
	vk::DeviceSize bufferSize;
	Memory allocation;
};
