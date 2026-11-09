# Shader compilation and stutter: the plan

Goal: starting a game for the first time, with nothing kept from an
earlier run, plays the same as a run with everything kept. No shader
stutters, no pauses. The standing rules are in `AGENTS.md`; this file
says where each renderer stands against them and the order the work is
done in. A phase is done when its CI test says so, not when the code
lands.

## Where the core stands

What every renderer already does right:

- Vulkan's and Direct3D 12's shaders are bytecode embedded in the core
  (SPIR-V, DXBC, DXIL), made by the `tools/rend/kernels_*.sh` scripts.
  OpenGL's programs are GLSL the driver compiles (its decoders' SPIR-V
  where `GL_ARB_gl_spirv` is there); Direct3D 9's and 11's are HLSL
  compiled with `D3DCompile` while the core runs.
- A polygon's own pipeline is made on a worker (Vulkan `VkPsoWorker`,
  Direct3D 11 and 12 `d3d_warm`) or by the driver's own threads (OpenGL
  with `KHR_parallel_shader_compile`), and is never waited for.
- Vulkan, OpenGL 3+ and the OpenGL 4 per-pixel renderer have a general
  pipeline or program that reads the polygon's key from push constants or
  a uniform, and draws while the polygon's own is made.
- The texture decoders, the TA decoders and Direct3D 12's writeout, HDR
  output and compute renderer are made when the renderer starts.

### Draw-path compiles still left

Ranked by how often they hit in play.

**Direct3D 11**

Polygons draw with a general pixel shader (the key in the polygon's
constants) while their own is compiled on the warm thread; every vertex
shader, fixed pixel shader and reachable state is made at the start
(`tools/rend/d3d_draw_path.sh` holds it at zero). Left:

1. A shader the warm thread made is kept for `d3d11_shaders.bin` by the
   draw thread (`TakeBuilt` → `Keep`): a copy and a map insert, no
   compile.
2. The shaders are still HLSL compiled while the core runs, so a
   system without `d3dcompiler_47.dll` (or with wine's, which does not
   compile the per-pixel renderer's) has fewer of them.
3. The TA decoder's kernels on an option change (phase 5).

**Vulkan**

The start (`context_reset`) waits, on the worker, for every fixed
pipeline its first frame needs; a device without
`VK_EXT_extended_dynamic_state` or `_state3` has the general pipeline of
every fixed state a draw can reach made then too (`Prepare()` runs `Key()`
over the drawer's draws: 130 to 394 a manager on lavapipe), and HDR
output's post effects come with it. `tools/vulkan/vk_draw_path.sh` holds
`retro_run` at zero. Left:

1. A device with neither extension: the states a draw can reach are
   thousands (18840 per-triangle, 7206 per-pixel, twice that with SSAA),
   over `PSO_GENERAL_AT_START`; those of earlier runs are made at the
   start, and one not seen before in the frame (`BuildSync`, about one a
   new state). Folding the blend into the shader as Direct3D 12 does (64
   pairs to 15, with dual-source blending) brings them under it.
2. Every resize (resolution, widescreen, SSAA, the upscaler's size):
   write-out compiled there (`VulkanWriteOut::Init`), the compute
   renderer's modules and kernels remade, SSAA resolve, the overlay quad
   and the motion vector pass dropped and remade, made by the frame that
   needs them first (`Need`).
3. Option changes: `PipelineManager::Reset` waits for the worker and the
   device; the HDR subpass pipeline is made with the render pass; the TA
   decoder's modules are made when it is turned on. A change of renderer
   (per-triangle, per-pixel, compute, SSAA) is done: the new one's
   pipelines are made on the worker while the frames show the kept
   picture and the machine waits (`Renderer::Ready()`, `retro_run()`).
4. A draw `Choose` has no pipeline for is skipped: the general table full,
   which only a device with neither extension can reach, or a build that
   failed.

**OpenGL and OpenGL 4**

Every version has a general program (OpenGL ES 2 and OpenGL 2 read the
key as four nibbles in floats), and `tools/gl/gl_draw_path.sh` holds
`retro_run` at no compile, link or binary load, its pictures those of the
general programs alone. Left:

1. A key first seen in a run is drawn with the general program for the
   rest of it, whatever the driver (no compile call in a frame, parallel
   compile or not); its own program is made at the next start.
2. Option changes inside `retro_run`: the scaler's write-out program, the
   TA decoder's kernels, and the whole renderer when it is switched.

**Direct3D 12**

1. A key the start did not make, or one a frame wants before the thread
   has got to it, is made in the frame (`DX12Pipelines::Missed`). Since
   the blend fold, none on the test discs; the start does not yet make
   every reachable key.

**Direct3D 9**

Nothing: its eight shaders are made at the start, and its states are
render states.

### Measurement in the repo

- Direct3D 12 counts pipelines made in a frame (`statMadeInFrame`).
- Vulkan counts pipelines `Need` and `BuildSync` made (`syncBuilt`), and
  `tools/vulkan/vk_draw_path.sh` fails on any made in `retro_run` (CI,
  cold, every renderer and disc, without each dynamic state, HDR; a
  device with neither extension allowed, see above).
- Direct3D 11 counts shaders and states made on the draw path and draws
  with a general shader, and `tools/rend/d3d_draw_path.sh` fails on any
  made there (CI, cold, every renderer and the threaded setting).
- OpenGL: `tools/gl/gl_draw_path.sh` fails on a glCompileShader,
  glLinkProgram or glProgramBinary in `retro_run` (CI, cold, OpenGL ES 2
  and 3, OpenGL 2 and 3+, on a driver that compiles in the background).
- Nothing fails a test on Direct3D 12 yet.

## Phase 0: measure (lands first)

1. Every renderer counts, where it happens, with one add: pipelines,
   programs, shaders and state objects made on the draw thread after
   boot; waits for a compile there; draws with no pipeline. Nothing is
   logged on the draw path.
2. One CI test per renderer runs the test discs cold (nothing kept),
   every sort mode, and reads the counters at the end. It fails on any
   count that is not on the renderer's allowed list, and the list only
   ever shrinks. A phase below takes its renderer's entries off the list
   in the same commit.

## Phase 1: Direct3D 11

Done, but for:

1. The polygons' shaders embedded as DXBC, made by
   `tools/rend/kernels_dxbc.sh`, as Direct3D 12's are: no compiler needed
   at run time.
2. The warm thread keeping what it compiled itself, so the draw thread
   only adopts a shader.

## Phase 2: Vulkan

Done, but for:

1. The blend folded into the general shaders (dual-source blending,
   where the device has it), so a device with neither extension has its
   reachable states made at the start too.
2. A pipeline remade for a resize made on the worker beside the old one,
   which draws until the new one is ready (write-out, the compute
   renderer, SSAA resolve, the overlay quad, the motion vector pass).
3. No skipped draws: the general table sized for every reachable key.

## Phase 3: OpenGL

1. Done: a general program for OpenGL ES 2 and OpenGL 2 (the key's
   nibbles in a float uniform).
2. Done: a new key's own program is not asked for in a frame on OpenGL 3+
   either (OpenGL makes programs only on a context's thread, and a core
   has only the frontend's): the general program draws it for the run,
   and its own is made at the next start.
3. Option changes: the scaler's write-out and the TA decoder's kernels
   made at the start whether on or not.

## Phase 4: Direct3D 12

1. The key made small enough for every reachable one to be made at the
   start. Done: the cull mode, in the polygons' and the per-pixel
   renderer's pixel shaders (their pipelines cull nothing; SV_IsFrontFace
   and discard, DX12_PF_CULL_SHIFT), and the shading, flat and Gouraud in
   one shader (the vertex shader's colours both ways, the pixel shader
   choosing by DX12_PF_GOURAUD): the start set 634 to 124, the per-pixel
   renderer's whole run 879 to 169. Left: the per-pixel renderer's blends
   folded as the per-triangle one's are (it still makes two or three in a
   frame).
2. Then every reachable key made before the first frame, the start held
   as Vulkan's change of renderer is (`Renderer::Ready()`), with nothing
   left for `Missed` to make.

Testing under wine: its vkd3d drops draws of the renderer's
ExecuteIndirect batches (root constants a draw), so the pictures of a
build that batches differently differ. `d3d_hdr`'s `D3DH_NO_INDIRECT` has
the core draw one draw at a time: then the per-triangle and per-pixel
renderers' pictures are Vulkan's on the plain, culling and DMA discs.
`tools/rend/d3d_same.sh` compares two builds that way, on those discs and
a Gouraud-shaded triangle's.

## Phase 5: option changes everywhere

Changing an option while a game runs (resolution, SSAA, the upscaler,
the renderer, GPU TA, write-out, HDR) makes what it needs on a worker
while the old setting keeps drawing, and switches when it is ready.

Vulkan's change of renderer does this: until the new renderer is ready,
`retro_run()` shows the picture kept from the old one and does not run
the machine. Direct3D 12 takes it once its key is small enough for its
start set to be made in a few frames (phase 4); Direct3D 11's start
compiles on the thread it is called on, and OpenGL has only the
frontend's context to compile in.
Direct3D 11's write-out and HDR post effects are made at the start
already; its TA decoder's kernels are not.
