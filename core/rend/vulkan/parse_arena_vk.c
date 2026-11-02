/* See parse_arena_vk.h. C89. */
#include <string.h>
#include "parse_arena_vk.h"

/* What the parse's memory has to be: the device's own (drawn from where it
 * is), mapped, coherent (nothing flushed), and in the host's cache - the
 * parse and the sort read back what they write, which memory the host
 * writes through, uncached, makes slow */
#define PARSE_MEMORY (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT \
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)

int parse_arena_vk_init(parse_arena_vk_t *a, VkDevice device, vk_heap_t *heap, size_t vert_room, size_t idx_room,
      unsigned slices)
{
   VkBufferCreateInfo info;
   VkMemoryRequirements req;
   size_t bytes;

   memset(a, 0, sizeof(*a));
   /* (a slice's indices 256 bytes aligned: more than an index buffer's
    * offset needs, and the sorted passes' after them are as aligned as
    * the frame's indices' end, 4 bytes) */
   bytes = parse_arena_layout(&a->arena, vert_room, idx_room, slices, 256);
   if (bytes == 0)
      return 0;
   memset(&info, 0, sizeof(info));
   info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   info.size        = bytes;
   info.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   if (vkCreateBuffer(device, &info, NULL, &a->buffer) != VK_SUCCESS)
   {
      a->buffer = VK_NULL_HANDLE;
      return 0;
   }
   a->device = device;
   a->heap = heap;
   vkGetBufferMemoryRequirements(device, a->buffer, &req);
   if (!vk_heap_alloc(heap, &req, PARSE_MEMORY, 0, 1, &a->mem))
   {
      memset(&a->mem, 0, sizeof(a->mem));
      parse_arena_vk_term(a);
      return 0;
   }
   if (a->mem.mapped == NULL || vkBindBufferMemory(device, a->buffer, a->mem.memory, a->mem.offset) != VK_SUCCESS)
   {
      parse_arena_vk_term(a);
      return 0;
   }
   parse_arena_bind(&a->arena, a->mem.mapped);
   return 1;
}

void parse_arena_vk_term(parse_arena_vk_t *a)
{
   parse_arena_detach(&a->arena);
   if (a->buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(a->device, a->buffer, NULL);
   if (a->mem.memory != VK_NULL_HANDLE)
      vk_heap_free(a->heap, &a->mem);
   memset(a, 0, sizeof(*a));
}
