# Allocations during play

AGENTS.md, "No allocations during play": once a game runs, nothing is
allocated on the render thread, in `retro_run` or on the emulation
thread. This is where the core stands, and the order the rest goes in.

## Measured

`tools/vulkan/vk_allocs.sh` (CI: `vulkan-negotiation.yml`) runs the core
under a Vulkan frontend (`vk_hdr.c`, `VKH_ALLOCS`) on the test discs with
every Vulkan renderer and counts, from frame 20 on, what the core
allocates: the heap (malloc and the rest, operator new, from the core's
code or through the C and C++ runtimes for it; a driver's own do not
count) and Vulkan memory, buffers, images and pools. Each place is named
by the function that asked (`vk_alloc_sites.py`; any of the first three
the core's own, so that inlining does not move it) and must be on
`tools/vulkan/vk_allocs.allowed`. The list only ever shrinks. A site
reached from the core's signal handler fails whatever the list says.

Where it started (60 frames of each test disc, frame 20 on): about 5000
to 6000 heap allocations a run, some 80 a frame, nearly all of them the
SH-4 dynarec's; 9 buffers, 8 to 14 images and 4 command pools.

## In order

1. **The signal handler.** Done: nothing is allocated there. The read
   back of a render to a texture (`rtt_watch_fault()` -> `vk_rtt_read()`)
   has its batches made with the renderer and every copy's room grown
   when a render is watched (`copies_reserve()`); the dynarec's write to
   a page of code (`bm_RamWriteAccess()`) discards the page's blocks
   without copying their list, into a list of discards reserved at
   `bm_Init()`. Left of it: the copies' growth when a larger render is
   first watched (`vk_rtt_watch`), at the bound later.
2. **A frame's draws.** Done: a descriptor set is made with nothing
   allocated but the set (`vk_alloc_desc_set()`: vulkan.hpp's
   allocateDescriptorSetsUnique() made a std::vector for each), a retired
   one is given back through the command pool's list (`DeferFree()`); a
   drawer's main and vertex uniform buffers, and its texture descriptor
   cache's tables and first sets, are made with it; every sampler a TSP
   can ask for (288) is made at the renderer's start
   (`SamplerManager::Init()`); the translucent sort's room is reserved
   before the first frame (`SortReserve()`, `SortPassesReserve()`: the
   translucent list's polygons, 64K triangles). Left: the quad's set made
   again from the pool each frame the image changes (no heap: a set kept
   for each of the frontend's images would make none).
3. **Renders to textures.** Their targets, framebuffers and buffers
   (`TextureDrawer::BeginRenderPass`, `BufferData`, `FramebufferAttachment`,
   the per-pixel renderer's) from a pool made at the start, at the bound
   the video memory budget gives.
4. **The texture cache.** A texture first seen takes its picture from
   the pool sized at the start (`TexturePool::Make`, and its log line);
   the cache's map, its pieces' hashes, the samplers and the video memory
   locks (`vramlock_list_add`) in arenas of their bound.
5. **The dynarec.** The pages' lists of their blocks and the temporary
   blocks' list are the blocks' own links now (no std::set: nothing
   allocated to put a block in or take it out). Left: blocks compiled
   while the game runs (`ngen_Compile`, `BlockCompiler`, `bm_AddBlock`,
   Xbyak's labels): the block's opcodes
   into a scratch arena reset for each block, its bookkeeping in a pool
   sized for the code cache, the assembler's labels in fixed tables.

Other renderers (OpenGL, Direct3D) get the same measure once their
harnesses count it.
