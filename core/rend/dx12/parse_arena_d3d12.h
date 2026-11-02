#ifndef PARSE_ARENA_D3D12_H
#define PARSE_ARENA_D3D12_H
/* The parse's arena (rend/parse_arena.h) with Direct3D 12: one buffer of
 * a custom heap with the processor's own, cached pages (write-back) in the
 * memory the card reads, mapped for good, in slices for a few frames in
 * flight, where the frame is parsed and then drawn from: no copy into the
 * upload ring. Only where the adapter's memory is the processor's, kept
 * coherent with its cache (CacheCoherentUMA): elsewhere such pages are
 * read over the bus, and the frame is copied, as ever.
 *
 * A slice is taken when the context's fence has passed the value it was
 * last drawn with (polled: nothing waits); with none free, the frame is
 * copied. C89. */
#include <d3d12.h>
#include "../parse_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parse_arena_d3d12
{
   parse_arena_t arena;
   ID3D12Resource *buffer;
   D3D12_GPU_VIRTUAL_ADDRESS gpu;
   ID3D12Fence *fence;                 /* the context's (not this one's to release) */
   UINT64 drawn[PARSE_ARENA_SLOTS];    /* the fence's value once a slice is done with */
   unsigned next;                      /* the slice looked at first */
   int ready;                          /* (all zero, as a static one is before it is made: none) */
} parse_arena_d3d12_t;

/* Made on @device for @slots frames of lists of the rooms @room (by
 * PARSE_ARENA_*), its slices free when @fence says so; false, and nothing
 * kept, where the adapter is not CacheCoherentUMA or the buffer cannot be
 * made and mapped */
int parse_arena_d3d12_init(parse_arena_d3d12_t *a, ID3D12Device *device, ID3D12Fence *fence,
      const size_t *room, unsigned slots);
/* (the frames drawn from it done on the card; @buffer is given to
 * @release where the context lets it go later, or released now) */
void parse_arena_d3d12_term(parse_arena_d3d12_t *a, void (*release)(void *arg, IUnknown *object), void *arg);

/* The frame whose lists these are (@lists, by PARSE_ARENA_*; @vert_head as
 * parse_arena_attach()'s) parsed into a free slice, till
 * parse_arena_d3d12_drawn() or parse_arena_d3d12_cancel(); false where
 * none is free or the lists are not of the arena's rooms */
int parse_arena_d3d12_attach(parse_arena_d3d12_t *a, ta_list_t *const *lists, size_t vert_head);
/* ...drawn by the commands the fence's @value is signalled after: its
 * slice the card's till then, and its lists its own again */
void parse_arena_d3d12_drawn(parse_arena_d3d12_t *a, UINT64 value);
/* ...not drawn: its lists its own again, and its slice free */
void parse_arena_d3d12_cancel(parse_arena_d3d12_t *a);

/* Where part @part of the frame's slice is, for the card; 0 when no frame
 * is attached */
D3D12_GPU_VIRTUAL_ADDRESS parse_arena_d3d12_gpu(const parse_arena_d3d12_t *a, unsigned part);

#ifdef __cplusplus
}
#endif

#endif
