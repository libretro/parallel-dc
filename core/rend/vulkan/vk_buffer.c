/* See vk_buffer.h. */
#include <string.h>
#include "vk_buffer.h"

#define HOST_COHERENT (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

VkResult vk_buffer_init(vk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props)
{
	VkBufferCreateInfo info;
	VkResult res;
	memset(b, 0, sizeof(*b));
	b->size = size;
	b->props = props;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	res = vkCreateBuffer(vkc.device, &info, NULL, &b->buffer);
	if (res != VK_SUCCESS)
	{
		b->buffer = VK_NULL_HANDLE;
		return res;
	}
	if (!vkc_heap_alloc_buffer(b->buffer, !(props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
			(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0, &b->alloc))
	{
		vkDestroyBuffer(vkc.device, b->buffer, NULL);
		b->buffer = VK_NULL_HANDLE;
		return vkc_heap_error();
	}
	b->allocated = true;
	return VK_SUCCESS;
}

void vk_buffer_free(vk_buffer *b)
{
	if (b->buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(vkc.device, b->buffer, NULL);
	b->buffer = VK_NULL_HANDLE;
	if (b->allocated)
		vk_heap_free(&vkc.heap, &b->alloc);
	b->allocated = false;
}

void vk_buffer_upload(const vk_buffer *b, u32 size, const void *data, u32 offset)
{
	verify((b->props & HOST_COHERENT) == HOST_COHERENT);
	verify(offset + size <= b->size);
	memcpy((u8 *)vk_buffer_map(b) + offset, data, size);
}

void vk_buffer_upload_many(const vk_buffer *b, size_t count, const u32 *sizes, const void **data, u32 offset)
{
	u8 *p;
	u32 total = 0;
	size_t i;
	verify((b->props & HOST_COHERENT) == HOST_COHERENT);
	for (i = 0; i < count; i++)
		total += sizes[i];
	verify(offset + total <= b->size);
	p = (u8 *)vk_buffer_map(b) + offset;
	for (i = 0; i < count; i++)
	{
		if (data[i] != NULL)
			memcpy(p, data[i], sizes[i]);
		p += sizes[i];
	}
}

void vk_buffer_download(const vk_buffer *b, u32 size, void *data, u32 offset)
{
	verify((b->props & HOST_COHERENT) == HOST_COHERENT);
	verify(offset + size <= b->size);
	memcpy(data, (const u8 *)vk_buffer_map(b) + offset, size);
}
