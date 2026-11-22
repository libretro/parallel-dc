/* A buffer and its memory from the context's heap (vk_context.h) */
#ifndef VK_BUFFER_H
#define VK_BUFFER_H

#include "vk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_buffer
{
	VkBuffer buffer;
	VkDeviceSize size;
	vk_heap_alloc_t alloc;
	bool allocated;
	VkMemoryPropertyFlags props;	/* asked for */
} vk_buffer;

/* @size bytes for @usage; host visible unless @props has device local.
 * VK_SUCCESS, or why not (and @b empty). */
VkResult vk_buffer_init(vk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
void vk_buffer_free(vk_buffer *b);

/* The memory, mapped for the life of the buffer. NULL if it is not host visible. */
static INLINE void *vk_buffer_map(const vk_buffer *b)
{
	return b->allocated ? b->alloc.mapped : NULL;
}

/* What kind of memory it has (VkMemoryPropertyFlags) */
static INLINE VkMemoryPropertyFlags vk_buffer_memory_flags(const vk_buffer *b)
{
	return b->allocated ? vkc.heap.props.memoryTypes[vkc.heap.blocks[b->alloc.block].type].propertyFlags : 0;
}

void vk_buffer_upload(const vk_buffer *b, u32 size, const void *data, u32 offset);
/* @count pieces one after the other; a NULL one's room is left as it is */
void vk_buffer_upload_many(const vk_buffer *b, size_t count, const u32 *sizes, const void **data, u32 offset);
void vk_buffer_download(const vk_buffer *b, u32 size, void *data, u32 offset);

#ifdef __cplusplus
}
#endif

#endif
