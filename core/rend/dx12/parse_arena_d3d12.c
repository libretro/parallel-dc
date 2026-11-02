/* See parse_arena_d3d12.h. C89. */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <string.h>
#include "parse_arena_d3d12.h"

/* (ID3D12Resource's, here: nothing is linked for it) */
static const GUID resource_iid = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

int parse_arena_d3d12_init(parse_arena_d3d12_t *a, ID3D12Device *device, ID3D12Fence *fence,
      const size_t *room, unsigned slots)
{
   D3D12_FEATURE_DATA_ARCHITECTURE arch;
   D3D12_HEAP_PROPERTIES heap;
   D3D12_RESOURCE_DESC desc;
   D3D12_RANGE none;
   void *map = NULL;
   size_t bytes;

   memset(a, 0, sizeof(*a));
   memset(&arch, 0, sizeof(arch));
   if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)))
         || !arch.CacheCoherentUMA)
      return 0;
   /* (a slice's parts 256 bytes aligned: more than a vertex or index
    * buffer's view needs) */
   bytes = parse_arena_layout(&a->arena, room, slots, 256);
   if (bytes == 0)
      return 0;
   memset(&heap, 0, sizeof(heap));
   heap.Type = D3D12_HEAP_TYPE_CUSTOM;
   heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
   heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
   memset(&desc, 0, sizeof(desc));
   desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   desc.Width = bytes;
   desc.Height = 1;
   desc.DepthOrArraySize = 1;
   desc.MipLevels = 1;
   desc.SampleDesc.Count = 1;
   desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
         D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &resource_iid, (void **)&a->buffer)))
   {
      a->buffer = NULL;
      return 0;
   }
   /* (nothing read through the mapping yet) */
   none.Begin = 0;
   none.End = 0;
   if (FAILED(ID3D12Resource_Map(a->buffer, 0, &none, &map)) || map == NULL)
   {
      ID3D12Resource_Release(a->buffer);
      a->buffer = NULL;
      return 0;
   }
   parse_arena_bind(&a->arena, map);
   a->gpu = ID3D12Resource_GetGPUVirtualAddress(a->buffer);
   a->fence = fence;
   a->ready = 1;
   return 1;
}

void parse_arena_d3d12_term(parse_arena_d3d12_t *a, void (*release)(void *arg, IUnknown *object), void *arg)
{
   if (a->ready)
      parse_arena_detach(&a->arena);
   if (a->buffer != NULL)
   {
      if (release != NULL)
         release(arg, (IUnknown *)a->buffer);
      else
         ID3D12Resource_Release(a->buffer);
   }
   memset(a, 0, sizeof(*a));
}

int parse_arena_d3d12_attach(parse_arena_d3d12_t *a, ta_list_t *const *lists, size_t vert_head)
{
   UINT64 done;
   unsigned i;

   if (!a->ready || a->arena.slot >= 0)
      return 0;
   done = ID3D12Fence_GetCompletedValue(a->fence);
   for (i = 0; i < a->arena.slices; i++)
   {
      const unsigned s = (a->next + i) % a->arena.slices;
      if (a->drawn[s] > done)
         continue;
      if (!parse_arena_attach(&a->arena, s, lists, vert_head))
         return 0;
      a->next = (s + 1) % a->arena.slices;
      return 1;
   }
   return 0;
}

void parse_arena_d3d12_drawn(parse_arena_d3d12_t *a, UINT64 value)
{
   if (!a->ready || a->arena.slot < 0)
      return;
   a->drawn[a->arena.slot] = value;
   parse_arena_detach(&a->arena);
}

void parse_arena_d3d12_cancel(parse_arena_d3d12_t *a)
{
   if (a->ready)
      parse_arena_detach(&a->arena);
}

D3D12_GPU_VIRTUAL_ADDRESS parse_arena_d3d12_gpu(const parse_arena_d3d12_t *a, unsigned part)
{
   if (!a->ready || a->arena.slot < 0 || part >= PARSE_ARENA_PARTS)
      return 0;
   return a->gpu + parse_arena_offset(&a->arena, (unsigned)a->arena.slot, part);
}
