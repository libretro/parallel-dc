# AGENTS.md

Instructions for anyone, human or agent, changing this core. Read the
section that covers the code you touch before touching it.

## Shader compilation and stutter

This comes first. The target is the console's own behaviour: a game never
waits for a shader. Starting a game for the first time, with nothing kept
from an earlier run, must play the same as a run with everything kept.
Nobody should be able to see a shader stutter or a pause. Compilation gets
better with every commit, and never worse.

Where each renderer stands, and the order its draw-path compiles are
removed in, is `docs/ShaderStutterPlan.md`. Keep it current: a commit
that removes one takes it off the list there.

### Invariants

1. **Nothing compiles on the draw path.** After boot, no shader, program
   or pipeline is created on the thread that renders a frame or inside
   `retro_run`, and nothing there waits for one to compile: no
   `vkCreateGraphicsPipelines`/`vkCreateComputePipelines`/
   `vkCreateShaderModule`, no `glCompileShader`/`glLinkProgram`/
   `glProgramBinary` or a link status read of a program still linking,
   no `Create*PipelineState`, no `Create*Shader`. This holds for every
   renderer (OpenGL, OpenGL 4 per-pixel, Vulkan, Vulkan per-pixel, the
   compute renderers, Direct3D 9, 11 and 12) and every pipeline they own:
   the polygons', modifier volumes', quads', the texture and TA decoders',
   writeout, SSAA resolve, the upscaler and the HDR output. A code path
   that adds one is a bug, even when it runs only the first time a state
   is seen, and even when it is logged.
2. **Every draw has a correct pipeline ready.** A draw whose specialized
   pipeline is not made yet uses one that renders the same result: a
   general pipeline that reads its key from push constants, root
   constants or uniforms. Never a skipped draw, a placeholder or a
   guessed approximation. What a general pipeline cannot take as a
   constant (Direct3D 12's blend, depth and cull state) is made for every
   reachable combination before the first frame. Before a key loses a
   field to a constant or to dynamic state, show that the constant
   version renders the same (`tools/rend/dx12_blend_fold_test.sh` is the
   pattern).
3. **Workers never hold up a draw.** A full queue defers speculative
   work, and the draw goes ahead with its ready pipeline. Finished work
   is adopted on the thread that owns the cache, without waiting.
   Shutdown stops the producers, cancels queued work, finishes active work
   and drains results before any device object is destroyed. Workers are
   lock-free (retro_atomic, SPSC rings, eventcount park and wake): parking
   one is fine, busy-spinning one or waiting on the draw path for a free
   slot is not.
4. **The disk cache is never what removes a stutter.** Invariants 1 and
   2 hold with nothing kept from an earlier run. Where a cache is kept
   (the Vulkan pipeline cache, the OpenGL program binaries, the Direct3D
   12 pipeline library):
   - nothing reads or writes the disk on the render thread or in
     `retro_run`; what is new collects in memory and is written at
     shutdown or by a worker;
   - files live in the frontend's system directory;
   - a corrupt or mismatched file is a recoverable miss, never a crash.
5. **Zero-copy in the shader path.** Shader bytecode is embedded once
   (the `*_spv.inc`, `*_dxbc.inc`, `*_dxil.inc` files) and handed to the
   driver in place. Cache files are mapped or read in place through
   libretro-common's filestream/VFS, and validated slices go straight to
   the driver (`vkCreatePipelineCache` initial data, `glProgramBinary`,
   `CreatePipelineLibrary`). No intermediate copies, and no heap
   allocation for shader work during play.
6. **Stable identity.** Keys are packed words compared exactly, with the
   bits a pipeline does not read cleared, folded the same way wherever a
   key is made (`DX12PsoKey::Pack()`). Never key on a pointer. A kept
   file names the bytecode it was made with (a sum of the shaders) and
   the key layout's version; changing what a key means bumps the version
   in the same commit.

### Measured, every commit

A commit that touches a renderer, its shaders or its pipeline caches
says in its delivery what it did to these numbers. They don't depend on
the machine, so they must not get worse:

- pipelines made on the draw path after boot, per renderer: 0;
- waits for a compile on the draw path: 0;
- pipelines made at the start, and how many of them a run used;
- pictures the same as the parent's (`tools/rend/d3d_same.sh`, and the
  Vulkan and OpenGL picture tests), with no validation errors.

Each renderer counts the first two where they happen (one add, nothing
logged on the draw path), and its CI test fails on a count not on that
renderer's allowed list, run on the test discs cold. The list only ever
shrinks (`docs/ShaderStutterPlan.md`, phase 0).

### Shaders are generated, never edited

The embedded bytecode is made by the tools from its source, with the
compiler each file records: `tools/rend/kernels_spv.sh` (SPIR-V),
`tools/rend/kernels_dx12.sh` (Direct3D 12 DXBC and DXIL),
`tools/rend/kernels_dxbc.sh` (Direct3D 11 and the shared kernels). A
change to a shader source regenerates its include in the same commit,
and `check` passes. Where a mode exists to make one include alone
(`update-raster`, `update-dxil`, `update-hdr`), use it, so the others'
bytes stay as they are.

## No allocations during play

As with compiles, a game never waits for memory. Once a game runs,
nothing is allocated mid-game: not on the thread that renders a frame,
not in `retro_run`, not on the emulation thread.

- **What counts.** Heap allocation of any kind (`malloc`, `new`, a
  container growing, a string built), and the GPU's: buffers, textures,
  heaps, descriptor heaps and pools, command allocators, memory
  (`vkAllocateMemory`, `vkCreateBuffer`/`vkCreateImage`,
  `CreateCommittedResource`/`CreatePlacedResource`/`CreateHeap`,
  `CreateDescriptorHeap`, D3D11's `Create*`, `glBufferData` that resizes,
  `glTexImage*` that defines a new level). Freeing during play is the
  same work and counts too.
- **Sized before the first frame.** Everything play needs is made at
  load or context reset, at its bound: pools, arenas, rings, free lists,
  sized from what the hardware can hold (VRAM, the TA's lists, the
  texture cache's budget). Play takes from them and gives back.
- **A full pool is handled, not grown.** Evict, recycle or defer to the
  next frame; never allocate on the spot, and never drop a draw.
- **Changing an option** sets up what the new setting needs between
  frames, with the machine not running, the way a renderer change makes
  its pipelines (`Renderer::Ready()`), and not in the frame drawn next.
- **Measured.** Allocations during play are counted
  (`tools/vulkan/vk_allocs.sh`): every place that still allocates is on
  its list, which only ever shrinks. Where the core still allocates, and
  the order the rest goes in, is `docs/AllocationPlan.md`; a commit that
  touches such code removes its allocation.

## MSVC C89, gradually

New code is C89 that MSVC compiles, and code touched in C++ files moves
toward it, a piece at a time. No big-bang rewrite: each change leaves the
code it touches closer, and never adds C++ that was not there.

- **New files are C89.** `.c` files, test harnesses and tools included:
  declarations at the top of a block, `/* */` comments, no designated
  initializers, compound literals, VLAs or `//`.
- **Code touched in C++ files is written C89-shaped.** A struct and
  functions in place of a class, a switch on a kind in place of a
  virtual call, an explicit init and free in place of a constructor and
  destructor, no exceptions and no RTTI. Its codegen is checked to be
  faster than the C++ form, or at worst neutral.
- **No STL in the code a change touches.** Fixed arrays where a bound is
  known, a pointer, count and capacity grown by doubling where it is
  not, a function pointer and context in place of a callable. A list
  that is cleared keeps its storage, and its bound is reserved before
  play (No allocations during play).
- **Frames stay small.** A function's stack frame is 4 KiB at most.
- **Threads are lock-free.** retro_atomic, SPSC rings and eventcount park
  and wake; no mutexes, no sleeps or polling with a wall clock.
- **libretro-common is a plain copy** of RetroArch's
  `libretro-common/`. No core-specific edits there: adaptations go in
  the core's own files.

## Zero-copy and borrowed lends

Every change looks for copies to remove in the code it touches, and
every delivery says what it found and removed. A copy that stays has a
reason a reader can see.

- **One owner, the rest borrow.** Data is owned once. Everything else
  takes a pointer and a length for a bounded time, and the borrow ends
  before the owner frees or reuses the memory: a borrowed slice outlives
  every consumer, including the driver call it is passed to.
- **Producers write where the data ends up.** The TA parse writes
  vertices and indices into the memory the GPU reads (the parse arena),
  texture decoders into the upload the backend lends, a frame into the
  frontend's buffer. No intermediate buffer.
- **No slurps.** Files are mapped, or read in place through
  libretro-common's filestream/VFS.
- **Copies that must stay say why**, with a comment naming the reason:
  guest memory the GPU reads later is snapshotted as the emulated CPU
  goes on writing it; data an API hands over only for the length of a
  call.
- **Measure it.** Bytes copied and allocations made are work results:
  count them where they happen, and say what changed. They are not
  frame-time results until timing shows it.

## Measuring honestly

- **Clean runs.** Time on an idle machine, alternating parent and
  candidate (ABBA). Timing runs take no screenshots, picture hashes or
  readbacks; picture checks are separate runs. Report medians and the
  spread, never a single run.
- **Explain hitches with counters.** Don't diagnose a hitch by how it
  looks. A long frame is a compile only when the compile counters put
  one at that frame. Readbacks, guest CPU work and I/O look the same.
- **Counters cost nothing on the draw path.** One add where the event
  happens; no logging, no wall clocks there.
- **Count useful work.** A run that repeats frames or skips guest work
  only looks faster: check the machine state matches (a save state at a
  fixed frame) when emulation code changes.
- **Keep work and time apart.** Fewer compiles, fewer bytes copied and
  fewer allocations are work results. They are not frame-time results
  until timing shows it.

## Tests and delivery

- **A regression fix ships with its test**, wired into a workflow in
  `.github/workflows`, that fails on the broken code and passes on the
  fix. Show it fails: break the fix on purpose and run it.
- **A flaky test is fixed**, not rerun.
- **Every workflow that covers what a change touches passes** before it
  is delivered: `headless.yml`, `sanitizers.yml` (AddressSanitizer,
  UndefinedBehaviorSanitizer and LeakSanitizer, `make clean` between
  them), `vulkan-negotiation.yml` (the Vulkan, OpenGL and Direct3D tests
  under lavapipe and wine), `old-stl.yml`, `chd.yml`.
- **Every platform.** A touched C++ file also builds for Windows with
  mingw-w64, for AArch64, and is checked in an Apple shape; the
  buildbot's targets (macOS, iOS, Android, Switch) build from the same
  Makefile.
- **No game names or disc images** in files, comments or messages. The
  test discs are generated (`tools/threads/live_disc.py`).
