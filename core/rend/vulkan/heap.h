/*
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
#include "vk_context.h"

/* The C++ renderers' view of the context's heap (vk_context.h), until
 * they are C too */
class Allocation
{
public:
	Allocation() : heap(nullptr) {}
	Allocation(const Allocation& other) = delete;
	Allocation& operator=(const Allocation& other) = delete;

	Allocation(Allocation&& other) : heap(other.heap), alloc(other.alloc) {
		other.heap = nullptr;
	}

	Allocation& operator=(Allocation&& other) {
		std::swap(this->heap, other.heap);
		std::swap(this->alloc, other.alloc);
		return *this;
	}

	~Allocation() {
		if (heap != nullptr)
			vk_heap_free(heap, &alloc);
	}
	bool IsHostVisible() const {
		return heap != nullptr && alloc.mapped != nullptr;
	}
	/* The memory, mapped for the life of the allocation. NULL if it is not host visible. */
	void *MapMemory() const {
		return heap != nullptr ? alloc.mapped : nullptr;
	}
	/* What kind of memory it is (VkMemoryPropertyFlags) */
	VkMemoryPropertyFlags PropertyFlags() const {
		return heap != nullptr ? heap->props.memoryTypes[heap->blocks[alloc.block].type].propertyFlags : 0;
	}

private:
	Allocation(vk_heap_t *heap, const vk_heap_alloc_t& alloc) : heap(heap), alloc(alloc) {}

	vk_heap_t *heap;
	vk_heap_alloc_t alloc;

	friend class VulkanHeap;
};

class VulkanHeap
{
public:
	void TrimSoon() { vkc_heap_trim_soon(); }

	Allocation AllocateForImage(vk::Image image, bool linear, bool hostVisible, bool transient = false)
	{
		vk_heap_alloc_t alloc;
		if (!vkc_heap_alloc_image((VkImage)image, linear, hostVisible, transient, &alloc))
			throwResultException((vk::Result)vkc_heap_error(), "vk_heap_alloc failed");
		return Allocation(&vkc.heap, alloc);
	}

	Allocation AllocateForBuffer(vk::Buffer buffer, bool hostVisible, bool readBack)
	{
		vk_heap_alloc_t alloc;
		if (!vkc_heap_alloc_buffer((VkBuffer)buffer, hostVisible, readBack, &alloc))
			throwResultException((vk::Result)vkc_heap_error(), "vk_heap_alloc failed");
		return Allocation(&vkc.heap, alloc);
	}

	/* the heap itself, for what allocates from it in C */
	vk_heap_t *Raw() { return &vkc.heap; }
};
