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

The options that change what is drawn, or at what size, are measured on
the plain disc with each API that has them: texture upscaling at 2x and
4x, renders to a texture 2x and 4x finer, frame skipping, the internal
resolution, SSAA, anisotropic filtering, the PowerVR2 scaler, the vertical
screen, the VMU's screen, the crosshair, TA processing on the GPU, the
texture debug log, GPU Texture Decoding off, and the HDR output with its
post effects. Done for them: a texture scaled up (xBRZ) is drawn from the
32-bit images of the pool a size or two up, with its mipmaps made on the
GPU, which the pool is planned with (`TexPoolBook::Drawn()`); it is scaled
into room grown with the cache (`texcache_upscaled()`), whose scaler's
table and threads are made at the start. Its rows are shared out by bands
to a few threads of the core's own (`rend/band_pool.h`, in place of
OpenMP, which made and freed a team for each texture on one thread),
started with the cache and parked on an eventcount between textures. The
HDR output's intermediates for the post effects, and the pictures it hands
over, are kept for the last two sizes of picture (a game's frames may be
the framebuffer it writes and its renders in turn), not made again
whenever the size changes. With GPU Texture Decoding off, OpenGL converts
the textures into its upload ring, which is made when that is turned off.
Renders to a texture finer have their targets planned as large as they are
drawn too, and the per-pixel renderers' lists' heads (Vulkan) and images
(OpenGL 4) are as large as a render of a screen at that upscale from the
start. Left: texture upscaling at 6x and renders to a texture 3x and 8x
finer, whose images are of sizes no image of the pool's has (or larger
than it has); they are made when first drawn and kept in the cache.

## In order

1. **The signal handler.** Done: nothing is allocated there. The read
   back of a render to a texture (`rtt_watch_fault()` -> `vk_rtt_read()`)
   has its batches made with the renderer and every copy's room grown
   when a render is watched (`copies_reserve()`); the dynarec's write to
   a page of code (`bm_RamWriteAccess()`) discards the page's blocks
   without copying their list, into a list of discards reserved at
   `bm_Init()`. The copies' room is made with the renderer (item 3).
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
3. **Renders to textures.** Done: a render draws with the screen's
   attachments - the depth; the per-pixel renderer's colour and depth -
   which are made at least as large as a render of a screen, 640x480
   times Render To Texture Upscaling (`TextureDrawer::ShareExtent()`),
   and it draws its own pixels, not the whole texture they are in, whose
   rest is cleared black once (`Texture::ClearOutside()`). Nothing is
   made for a render, and no more video memory is taken than the screen's
   unless that upscaling is the higher; a larger render, which never
   comes from most games, has attachments of its own made when it comes
   (logged). With SSAA, or the upscaler, where the screen's attachments
   are not of a single sample or are kept past a frame, the texture
   drawers have their own, made with them (SSAA's as large as a render can
   be, `TextureDrawer::MaxSide()`, in memory made only when needed where
   SSAA is done to them). `tools/vulkan/vk_vram.sh` says how the renders
   were drawn and how much image memory there was at the most, and judges
   the black past a render. A framebuffer's place for each image; the
   room a render read back is put in, for one of the screen's size
   (`rtt_watch_reserve()`). The read back's copies take their room from
   one ring made with the renderer, four of a screen's render (4.7 MB of
   memory this side reads, in place of up to 18 renders' room grown as
   they came), and a finer render is made the console's size in one image
   for each of the four batches, not one for each copy (`vk_rtt_init()`,
   `copies_reserve()`); a render larger than a screen has them made larger
   when it is watched, which is said.
4. **The texture cache.** Done: the cache's textures are in room made
   when the renderer starts, twice as many as the pool has images, no
   fewer than 1024, found by a fixed index (`rend/tex_map.h`, in place of
   the `unordered_map`; `tools/rend/tex_map_test.sh`); the locks on video
   memory, and each page's list of them, in arenas made with it
   (`texcache_reserve()`); the pieces' hashes in arenas by size. The pool
   has eight 8x8 and four 16x16 textures of each format whatever their
   share (some 100 KB, which the share gave none of), and eight 64x64
   pictures rendered to. Each of these says, once, when it runs out and
   more is made. A line logged is formatted on the stack.
5. **The dynarec.** Done: the pages' lists of their blocks and the
   temporary blocks' list are the blocks' own links (no std::set). A
   block, its operations, its list of the blocks linked to it, its count
   (the shared pointer's), the map of blocks by their code, the self-
   modifying code's set and the assembler's labels (Xbyak's maps, given
   its allocator) take their room from one pool made when the recompiler
   starts, 8 MB, and give it back there (`node_pool.h`); the x64
   compiler is made in room of its own for each block, and a block's hash
   is taken on the stack. More room is made in chunks if a game needs it,
   which is said.

`tools/vulkan/vk_allocs.allowed` is empty: nothing in the core allocates
during play on the test discs with any Vulkan renderer.

With run-ahead a frontend saves and loads a state every frame, which is
play too: each measure runs the plain disc and the one whose state has a
render to a texture on the card in it with a state saved and loaded every
frame (`VKH_AHEAD`, `GLF_AHEAD`, `D3DH_AHEAD`). Done for it: a device on a
controller port that the state has one of the same type of is made anew
in its own memory, its map with it (`mcfg_Remake()`, `maple_CreateAt()`),
in place of sixteen made and freed for every state loaded.

OpenGL is measured the same way: `tools/gl/gl_allocs.sh`, the core under
`gl_frames.c` counting its heap and its OpenGL names, storage, programs
and shaders (`tools/rend/alloc_count.c`, shared with `vk_hdr.c`), against
`tools/gl/gl_allocs.allowed`. Done for it: a render read back takes its
room from one pixel buffer, a ring, and reads through framebuffers made
with the renderer (`rtt_gl_init()`; none in the signal handler); the
programs by key are in a fixed map (`rend/tex_map.h`); the frame's buffers
have their storage given at the start (`gl_buffer_room()`), and the
translucent sort its room (`gl_sort_reserve()`). OpenGL ES 3 has the
texture pool too (8-bit textures are `GL_R8`, as on OpenGL 3). Where
textures are converted on the CPU (OpenGL ES, OpenGL before 4.3) they are
converted into a piece of one pixel unpack buffer, a ring fenced a frame
at a time (`upload_ring_take()`), and nothing of the cache's is grown for
them. A device with next to no memory of its own (an integrated one that
says so) has the system's memory for its budget, and a figure no card has
is not taken for one. A rasterizer in software has the estimate: what
llvmpipe says of its memory is what was on the stack, another figure each
run, and the pool was planned from 22 MB in some runs and missed. The
per-pixel renderer's copy of the depth, for frames of more than one pass,
is made with its other pictures. `tools/gl/gl_allocs.allowed` is empty.

Direct3D 11 and 12 are measured the same way: `tools/rend/d3d_allocs.sh`,
the core's Windows build under wine on `d3d_hdr.c`, its heap counted
through what it imports for it (`tools/rend/alloc_count.c`'s Windows half:
msvcrt's, which the C++ runtime linked into it calls too, and the
system's) and the Direct3D objects it makes through the device's methods,
named by the build's symbol table, against `tools/rend/d3d_allocs.allowed`.
Done for it: a render read back takes its room from one readback buffer
(12) or staging picture (11), a ring made with the renderer, and nothing
is made in the fault handler; a texture's name for the log is written into
the caller's room (`GetId()`); the thread that makes shaders and pipelines
ahead is one, started with the renderer, which parks on an eventcount when
it has nothing to do (`rend/d3d_warm.h`); Direct3D 11's shader sources are
put together once and the bytecode it compiles kept as the compiler's blob
in a fixed map; and the texture pool has render targets four times as
long as they are wide. Left: a polygon's own Direct3D 11 pixel shader,
made on that thread when a frame first wants it (made at the start, it
would be every key's compiled before the first frame).
