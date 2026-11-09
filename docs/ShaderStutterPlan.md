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

1. Polygons' pixel shaders, both renderers (`DX11Shaders::GetPixelShader`,
   `DX11OITShaders::GetPixelShader` → `MissedPS`): a key not in
   `d3d11_shaders.bin` is compiled from HLSL (`D3DCompile`) and created in
   the frame. Every new combination on a first run, and every one after a
   shader source change. With no warm thread (a single-threaded device,
   no `HAVE_THREADS`) even the kept ones are created in the frame.
2. Depth-stencil, blend and sampler states (`DX11DepthStencilStates`,
   `DX11BlendStates`, `DX11Samplers`): made the first time a polygon needs
   one, about 290, 130 and 1024 reachable.
3. Vertex shaders: the flat-shaded one in both renderers, and the
   per-pixel renderer's modifier volume and final ones, are made on first
   use; vertex shaders are never warmed.
4. The modifier volumes' pixel shaders: warmed, but made in the frame if
   the thread has not got to them.
5. The HDR post effects (`hdr_d3d11_encode` → `post_make`): seven pixel
   shaders and a blend state in the frame they first turn on.
6. Write-out (`DX11WriteOut::Init`, compiled from HLSL each boot and on
   every resize) and the TA decoder's kernels (`OptionsChanged`): inside
   `retro_run` on an option change.

**Vulkan**

1. Devices without extended dynamic state: the general pipeline is keyed
   by blend (no EDS3) and by depth, stencil and cull (no EDS1), only the
   all-zero state is made at the start, and every other one is made in the
   frame (`PipelineManager::Choose` and the per-pixel one →
   `VkPsoWorker::BuildSync`).
2. `VkPsoWorker::Need`: a fixed pipeline (general, modifier volume,
   prepass, the per-pixel renderer's, quad, SSAA resolve, motion vectors,
   the kernels) the first frames want before the worker has made it is
   made there, or waited for.
3. Every resize (resolution, widescreen, SSAA, the upscaler's size):
   write-out compiled there (`VulkanWriteOut::Init`), the compute
   renderer's modules and kernels remade, SSAA resolve, the overlay quad
   and the motion vector pass dropped (waiting for the worker) and remade.
4. The HDR post effects (`hdr_vk_encode` → `post_make`): about eight
   pipelines in the frame they first turn on.
5. Option changes: `PipelineManager::Reset` waits for the worker and the
   device; the HDR subpass pipeline is made with the render pass; the TA
   decoder's modules are made when it is turned on.
6. A draw `Choose` has no pipeline for is skipped (the general table
   full, a build failed).

**OpenGL and OpenGL 4**

1. OpenGL ES 2 and OpenGL 2 have no general program: every new key is
   compiled, linked and waited for in the frame (`pipeline_first` →
   `CompilePipelineShader`).
2. OpenGL 3+ and the per-pixel renderer issue a new key's
   `glCompileShader` and `glLinkProgram` on the draw thread. Nothing waits
   (the general program draws, the driver's threads compile), but the
   calls are there, and a key kept from an earlier run is loaded with
   `glProgramBinary` and a link status read that waits.
3. Option changes inside `retro_run`: the scaler's write-out program, the
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
- Vulkan counts pipelines `Need` made itself (`syncBuilt`).
- Direct3D 11 counts the shaders its warm thread compiled (`statCompiledAhead`), not the ones a frame compiled.
- Nothing fails a test on any of them yet.

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

1. The polygons' shaders embedded as DXBC, made by
   `tools/rend/kernels_dxbc.sh`, as Direct3D 12's are: no compiler needed
   at run time, nothing compiled from HLSL while a game runs.
2. A general pixel shader, both renderers, reading the polygon's key from
   a constant buffer, made at the start and drawn with while the
   polygon's own is made on the warm thread.
3. Every reachable depth-stencil, blend and sampler state made at the
   start (they are small; the reachable sets are known).
4. Every vertex shader and every fixed pixel shader made at the start,
   with no warm-thread race: the start waits for what a first frame needs.
5. Write-out and the TA decoder made at the start whether on or not, the
   HDR post effects with the HDR output.

## Phase 2: Vulkan

1. With no extended dynamic state, the general pipeline made for every
   reachable blend, depth, stencil and cull state at the start (folding
   what can be folded, as Direct3D 12's blend is), so nothing is left to
   `BuildSync`.
2. `Need` never makes or waits: a fixed pipeline a frame wants is made
   before the first frame, and one remade for a resize is made beside the
   old one, which draws until the new one is ready.
3. Write-out, the compute renderer, SSAA resolve, the overlay quad and
   the motion vector pass remade on the worker on a resize, not in
   `retro_run`.
4. The HDR post effects made with the HDR output.
5. No skipped draws: the general table sized for every reachable key.

## Phase 3: OpenGL

1. A general program for OpenGL ES 2 and OpenGL 2 (the key's bits read
   from a float uniform where integers are not there).
2. A new key's own program on OpenGL 3+: OpenGL makes programs only on a
   context's thread, and a core has only the frontend's context. Either
   the calls stay, bounded (a few a frame, only with parallel compile, and
   never a wait), or the general program draws a key for the whole run and
   its own program is made at the next start. Which one is the user's
   decision; until then, the first.
3. Option changes: the scaler's write-out and the TA decoder's kernels
   made at the start whether on or not.

## Phase 4: Direct3D 12

1. Every reachable key made at the start, before the first frame (the
   start is boot), with the key folded further where a shader can take a
   field (flat and Gouraud shading in one shader); nothing left for
   `Missed` to make.

## Phase 5: option changes everywhere

Changing an option while a game runs (resolution, SSAA, the upscaler,
the renderer, GPU TA, write-out, HDR) makes what it needs on a worker
while the old setting keeps drawing, and switches when it is ready.
