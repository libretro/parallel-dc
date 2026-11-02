#ifndef PARSE_ARENA_VK_H
#define PARSE_ARENA_VK_H
/* The Vulkan renderers' parse arena (rend/parse_arena.h): one buffer, of
 * memory the device has as its own and the host has mapped, coherent and
 * in its cache. A device without such memory - a card on a bus, whose
 * memory the processor reads slowly or not at all, and whose processor's
 * memory is the card's far side - has none, and the frames are copied into
 * the drawers' buffers as before. C89. */
#include <vulkan/vulkan_symbol_wrapper.h>
#include "../parse_arena.h"
#include "vk_heap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parse_arena_vk
{
   parse_arena_t arena;
   VkDevice device;
   vk_heap_t *heap;
   VkBuffer buffer;
   vk_heap_alloc_t mem;
} parse_arena_vk_t;

/* Made for @slices frames in flight with lists of the rooms @room (by
 * PARSE_ARENA_*); false (and nothing kept) without the memory for it */
int parse_arena_vk_init(parse_arena_vk_t *a, VkDevice device, vk_heap_t *heap, const size_t *room, unsigned slices);
/* (the frames that used it done on the card) */
void parse_arena_vk_term(parse_arena_vk_t *a);

#ifdef __cplusplus
}
#endif

#endif
