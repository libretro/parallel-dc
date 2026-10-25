#!/bin/sh
# The Direct3D 12 renderer's shaders as bytecode, made here and committed,
# so that the core compiles none of them when it runs and a build of it
# needs no shader compiler:
#
#   core/rend/dx12/dx12_dxbc.inc  rend/dx12/shaders/dx12_raster.hlsl, each
#                                 shader with its defines and
#                                 rend/dx12/dx12_shader_flags.h in front
#                                 (SSAA's, with rend/tile/pvr_lane.h and
#                                 pvr_resolve.h before that)
#
#   core/rend/dx12/dx12_ta_dxbc.inc
#                                 GPU-side TA processing's kernels
#                                 (rend/ta_gpu.h with TAG_SHADER_HLSL and
#                                 its TAG_KERNEL; dx12/dx12_ta_decoder.cpp):
#                                 the header's GLSL kernels, which its
#                                 TAG_SHADER_HLSL words make HLSL of, with
#                                 what the preprocessor cannot change
#                                 changed here (ta_source_of())
#
#   core/rend/dx11/dx11_ta_dxbc.inc
#                                 the same kernels for the Direct3D 11
#                                 renderers (dx11/dx11_ta_decoder.cpp),
#                                 shader model 5.0, which Direct3D 11 takes:
#                                 the HLSL above with the buffers typed
#                                 views of 32-bit words, Buffer<uint> and
#                                 RWBuffer<uint>, in place of structured
#                                 ones - Direct3D 11 can draw from a
#                                 buffer, and take its indirect arguments
#                                 from it, only if it is not a structured
#                                 one - and no kernel past u7, as feature
#                                 level 11.0 has it (ta11_source_of())
#
#   core/rend/dx12/dx12_compute_dxil.inc
#                                 the compute renderer's kernels
#                                 (rend/tile/pvr_compute.h, after the shared
#                                 rules' headers, with PVRC_SHADER_HLSL and
#                                 its PVRC_KERNEL; dx12/dx12_compute.cpp):
#                                 the header's GLSL kernels made HLSL of the
#                                 same way (compute_source_of()), the walk
#                                 for 1, 2, 4 and 8 samples a pixel, in its
#                                 two variants (PVRC_V_SORTED, keeping
#                                 PVRC_KEEP_D3D12; PVRC_V_UNSORTED, keeping
#                                 1)
#
#   core/rend/dx12/dx12_hdr_dxbc.inc
#                                 the HDR output's own shaders of
#                                 dx12_raster.hlsl (HDR_SHADERS: the
#                                 per-pixel renderer's SSAA resolve that
#                                 stores the channel sums, DX12_OIT_SUMS),
#                                 of a compiler of its own; the
#                                 DX12_OIT_SUMS blocks are left out of the
#                                 text of the others
#
#   core/rend/dx12/dx12_compute_hdr_dxil.inc
#                                 the HDR output's walks of the compute
#                                 renderer (COMPUTE_HDR_KERNELS: the walk
#                                 for 1, 2, 4 and 8 samples, both variants,
#                                 that stores the channel sums, PVRC_SUMS),
#                                 of a dxc of its own; the PVRC_SUMS blocks
#                                 are left out of the text of the others
#
#   core/rend/dx12/dx12_compute_upscaler_dxil.inc
#                                 the temporal upscaler's walks of the
#                                 compute renderer (COMPUTE_UPSCALER_KERNELS:
#                                 the walk for 1 sample a pixel, both
#                                 variants, that stores each pixel's depth,
#                                 PVRC_DEPTH; and the two that store the
#                                 channel sums as well, for the HDR output
#                                 with it), of a dxc of its own; the
#                                 PVRC_DEPTH blocks are left out of the text
#                                 of the others
#
#   sh tools/rend/kernels_dx12.sh update   all made again from the sources
#   sh tools/rend/kernels_dx12.sh update-hdr  only the HDR output's DXBC made again
#   sh tools/rend/kernels_dx12.sh update-compute-hdr  ...and its DXIL
#   sh tools/rend/kernels_dx12.sh update-compute-upscaler  only the upscaler's DXIL made again
#   sh tools/rend/kernels_dx12.sh check    whether all are the sources' still
#   sh tools/rend/kernels_dx12.sh bindings what the compute kernels bind, from
#        their DXIL, as dx12/dx12_compute.cpp binds it (tools/rend/dxil_bindings.c)
#   sh tools/rend/kernels_dx12.sh compute-test DIR SAMPLES KEEP
#        the compute kernels for tools/rend/tile_test (TILE_DEVICE=d3d12):
#        test mode, nothing fused (PVRC_TEST, PVRC_EXACT), the walk for
#        SAMPLES and KEEP, as DIR/kK.dxil (the walk's sorted variant
#        DIR/k5_SAMPLES_KEEP.dxil, its unsorted DIR/k5u_SAMPLES.dxil;
#        TILE_PROD=1 in the environment: not test mode, for timing)
#
# DXBC, shader model 5.1 (descriptor tables indexed in the shader, for the
# bindless textures), which Direct3D 12 takes as it is: no DXIL, so no
# dxc either, but for the compute renderer's kernels, whose 64-bit integers
# shader model 5.1 has not: those are DXIL, shader model 6.0 and HLSL 2021,
# made by dxc (DXC=/path/to/dxc, with its libdxil.so, which signs them:
# Direct3D 12 takes no DXIL that is not), which the file names as it
# names d3dcompiler_47. Made with the same d3dcompiler_47.dll (D3DCompile,
# optimisation level 3) and tools/rend/dxbc_compile.c as the texture
# kernels (tools/rend/kernels_dxbc.sh), which the renderer takes from
# rend/texdecode_dxbc.inc as they are (shader model 5.0):
#   D3DCOMPILER=/path/to/d3dcompiler_47.dll  the compiler (needed to update)
#   CC_WIN=x86_64-w64-mingw32-gcc            what builds dxbc_compile.exe
#   WINE=wine                                what runs it ("" on Windows)
# The file says which d3dcompiler_47.dll made it (its SHA-1) and the SHA-1
# of each shader's text. check compares those sums, which needs no
# compiler; with the same compiler given, it compares every byte too.
# tools/rend/texdecode_test.sh runs check.
set -e
cd "$(dirname "$0")/../.."
INC=core/rend/dx12/dx12_dxbc.inc
SRC=core/rend/dx12/shaders/dx12_raster.hlsl
FLAGS=core/rend/dx12/dx12_shader_flags.h
LANE=core/rend/tile/pvr_lane.h
RESOLVE=core/rend/tile/pvr_resolve.h
# number:profile:defines (the numbers are dx12_shaders.h's DX12Shader)
SHADERS="1:vs_5_1:DX12_POLY_VS,DX12_GOURAUD=1 2:vs_5_1:DX12_POLY_VS,DX12_GOURAUD=0 3:ps_5_1:DX12_POLY_PS,DX12_GOURAUD=1 4:ps_5_1:DX12_POLY_PS,DX12_GOURAUD=0 5:vs_5_1:DX12_MODVOL_VS 6:ps_5_1:DX12_MODVOL_PS 7:vs_5_1:DX12_QUAD_VS 8:ps_5_1:DX12_QUAD_PS"
# the per-pixel renderer's: the vertex shaders, then a pixel shader for each pass and interpolation
SHADERS="$SHADERS 9:vs_5_1:DX12_OIT_VS,DX12_GOURAUD=1 10:vs_5_1:DX12_OIT_VS,DX12_GOURAUD=0"
SHADERS="$SHADERS 11:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=1,DX12_OIT_PASS=0 12:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=0,DX12_OIT_PASS=0"
SHADERS="$SHADERS 13:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=1,DX12_OIT_PASS=1 14:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=0,DX12_OIT_PASS=1"
SHADERS="$SHADERS 15:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=1,DX12_OIT_PASS=2 16:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=0,DX12_OIT_PASS=2"
SHADERS="$SHADERS 17:vs_5_1:DX12_OIT_FINAL_VS 18:ps_5_1:DX12_OIT_FINAL_PS 19:ps_5_1:DX12_OIT_MODVOL_PS"
# SSAA's (DX12_SSAA; a polygon's shaded at each sample, DX12_SAMPLE_RATE=1, or once a pixel, 2):
# the per-triangle renderer's polygons, smooth and flat at each rate
S="DX12_SSAA"
SHADERS="$SHADERS 20:ps_5_1:DX12_POLY_PS,$S,DX12_GOURAUD=1,DX12_SAMPLE_RATE=1 21:ps_5_1:DX12_POLY_PS,$S,DX12_GOURAUD=0,DX12_SAMPLE_RATE=1"
SHADERS="$SHADERS 22:ps_5_1:DX12_POLY_PS,$S,DX12_GOURAUD=1,DX12_SAMPLE_RATE=2 23:ps_5_1:DX12_POLY_PS,$S,DX12_GOURAUD=0,DX12_SAMPLE_RATE=2"
# the per-pixel renderer's: the depth and colour passes the same way, the lists' once a pixel
for pass in 0 1; do
   b=$((24 + pass * 4))
   SHADERS="$SHADERS $b:ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=1,DX12_OIT_PASS=$pass,DX12_SAMPLE_RATE=1"
   SHADERS="$SHADERS $((b + 1)):ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=0,DX12_OIT_PASS=$pass,DX12_SAMPLE_RATE=1"
   SHADERS="$SHADERS $((b + 2)):ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=1,DX12_OIT_PASS=$pass,DX12_SAMPLE_RATE=2"
   SHADERS="$SHADERS $((b + 3)):ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=0,DX12_OIT_PASS=$pass,DX12_SAMPLE_RATE=2"
done
SHADERS="$SHADERS 32:ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=1,DX12_OIT_PASS=2,DX12_SAMPLE_RATE=2 33:ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=0,DX12_OIT_PASS=2,DX12_SAMPLE_RATE=2"
# the lists put together and resolved, or put together at each sample; the translucent volumes; the resolve
SHADERS="$SHADERS 34:ps_5_1:DX12_OIT_FINAL_PS,$S,DX12_MS_TABLE 35:ps_5_1:DX12_OIT_FINAL_PS,$S,DX12_MS_TABLE,DX12_OIT_PER_SAMPLE"
SHADERS="$SHADERS 36:ps_5_1:DX12_OIT_MODVOL_PS,$S 37:ps_5_1:DX12_SSAA_RESOLVE_PS,$S,DX12_MS_TABLE"
# the lists' again, for NVIDIA's post-depth coverage (DX12_POST_Z_COVERAGE: pipelines made through NVAPI)
SHADERS="$SHADERS 38:ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=1,DX12_OIT_PASS=2,DX12_SAMPLE_RATE=2,DX12_POST_Z_COVERAGE"
SHADERS="$SHADERS 39:ps_5_1:DX12_OIT_PS,$S,DX12_GOURAUD=0,DX12_OIT_PASS=2,DX12_SAMPLE_RATE=2,DX12_POST_Z_COVERAGE"
# the temporal upscaler's motion vector pass (DX12_MOTION_VS, DX12_MOTION_PS)
SHADERS="$SHADERS 40:vs_5_1:DX12_MOTION_VS 41:ps_5_1:DX12_MOTION_PS 42:ps_5_1:DX12_MOTION_PS,DX12_MOTION_SAMPLED"
SHADERS="$SHADERS 43:ps_5_1:DX12_DEPTH_COPY_PS"
COUNT=43
# The HDR output's own, in core/rend/dx12/dx12_hdr_dxbc.inc (by DX12HdrShader):
# the per-pixel renderer's lists resolved with SSAA (34's) that store the
# channel sums as well
HDR_INC=core/rend/dx12/dx12_hdr_dxbc.inc
HDR_SHADERS="1:ps_5_1:DX12_OIT_FINAL_PS,$S,DX12_MS_TABLE,DX12_OIT_SUMS"
HDR_COUNT=1
TA_INC=core/rend/dx12/dx12_ta_dxbc.inc
TA11_INC=core/rend/dx11/dx11_ta_dxbc.inc
COMPUTE_INC=core/rend/dx12/dx12_compute_dxil.inc
COMPUTE_HDR_INC=core/rend/dx12/dx12_compute_hdr_dxil.inc
COMPUTE_UPSCALER_INC=core/rend/dx12/dx12_compute_upscaler_dxil.inc
PVRC_SRC=core/rend/tile/pvr_compute.h
PVRC_KERNELS=$(awk '$1 == "#define" && $2 == "PVRC_KERNELS" { print $3 }' $PVRC_SRC)
PVRC_K_WALK=$(awk '$1 == "#define" && $2 == "PVRC_K_WALK" { print $3 }' $PVRC_SRC)
# the kernels made, number:samples:variant (the walk's for each count it
# draws at, each of its two variants: pvr_compute.h's PVRC_V_*)
COMPUTE_KERNELS=
k=1
while [ $k -le "$PVRC_KERNELS" ]; do
   if [ $k = "$PVRC_K_WALK" ]; then COMPUTE_KERNELS="$COMPUTE_KERNELS $k:1:1 $k:2:1 $k:4:1 $k:8:1 $k:1:2 $k:2:2 $k:4:2 $k:8:2"; else COMPUTE_KERNELS="$COMPUTE_KERNELS $k:0:0"; fi
   k=$((k + 1))
done
# the temporal upscaler's: the walk's for 1 sample, variant:sums
COMPUTE_UPSCALER_KERNELS="1:0 2:0 1:1 2:1"
# ...a kernel's defines, and its name: the walk's for 1, d, u after it for
# the unsorted variant, s after that with the sums
upscaler_defines_of() {
   v=${1%%:*}
   if [ "$v" = 2 ]; then echo "#define PVRC_KEEP 1u"; else echo "#define PVRC_KEEP ${PVRC_KEEP_D3D12}u"; fi
   echo "#define PVRC_VARIANT ${v}u"
   echo "#define PVRC_DEPTH"
   if [ "${1##*:}" = 1 ]; then echo "#define PVRC_SUMS"; fi
}
upscaler_name_of() {
   echo "${PVRC_K_WALK}_1$( [ "${1%%:*}" = 2 ] && echo u)d$( [ "${1##*:}" = 1 ] && echo s)"
}
# the HDR output's: the walk's, with the channel sums
COMPUTE_HDR_KERNELS="$PVRC_K_WALK:1:1 $PVRC_K_WALK:2:1 $PVRC_K_WALK:4:1 $PVRC_K_WALK:8:1 $PVRC_K_WALK:1:2 $PVRC_K_WALK:2:2 $PVRC_K_WALK:4:2 $PVRC_K_WALK:8:2"
# a kernel's defines: @1 number:samples:variant
compute_defines_of() {
   v=${1##*:}
   if [ "$v" = 2 ]; then echo "#define PVRC_KEEP 1u"; else echo "#define PVRC_KEEP ${PVRC_KEEP_D3D12}u"; fi
   echo "#define PVRC_VARIANT ${v}u"
}
# ...and its name: number_samples, u after it for the unsorted variant
compute_name_of() {
   t=${1%:*}
   echo "${t%%:*}_${t#*:}$( [ "${1##*:}" = 2 ] && echo u)"
}
# (the translucent fragments the walk keeps at a time: pvrc_fragments_kept()
# of what Direct3D 12 gives a workgroup, 32 KiB)
PVRC_KEEP_D3D12=4
DXC_FLAGS="-T cs_6_0 -HV 2021 -E pvrc_entry -O3 -Qstrip_reflect -Qstrip_debug"
TA_SRC=core/rend/ta_gpu.h
TA_KERNELS=$(awk '$1 == "#define" && $2 == "TAG_KERNELS" { print $3 }' $TA_SRC)
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}
WINE=${WINE-wine}

WORK=${TMPDIR:-/tmp}/kernels_dx12.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# @1 the defines, a,b=1,...
source_of() {
   echo "$1" | tr ',' '\n' | while read d; do
      case "$d" in *=*) echo "#define ${d%%=*} ${d#*=}" ;; *) echo "#define $d" ;; esac
   done
   # (SSAA's resolve: the shared rule, rend/tile/pvr_resolve.h, as HLSL)
   case ",$1," in *,DX12_SSAA,*) echo "#define PVR_SHADER_HLSL"; echo "#define PVR_LANE_NONE"; cat $LANE $RESOLVE | grep -v "^#include" ;; esac
   # (the HDR output's blocks only in its own shaders: HDR_SHADERS)
   case ",$1," in
   *,DX12_OIT_SUMS,*) cat $FLAGS $SRC ;;
   *) cat $FLAGS; sed '/^#if defined(DX12_OIT_SUMS)$/,/^#endif \/\* DX12_OIT_SUMS \*\/$/d' $SRC ;;
   esac
}

# @1 a TA kernel (TAG_K_*): the header made HLSL of. The buffers' words
# "b.w[i]" are "b_w[i]"; "layout(local_size_x = N) in;" is the kernel's
# TAG_WG, and "void main()" its entry point with that many threads and
# GLSL's names for the invocation's numbers; "uint[3](...)" an array's "{...}"
ta_source_of() {
   echo "#define TAG_SHADER_HLSL"
   echo "#define TAG_KERNEL $1"
   sed -e 's/\b\(ta\|tab\|vtx\|mtr\|idx\|work\|rec\|cmd\|frm\|trd\|trp\|srt\)\.w\[/\1_w[/g' \
      -e 's/^layout(local_size_x = \(.*\)) in;$/#define TAG_WG \1/' \
      -e 's/^void main()$/[numthreads(TAG_WG, 1, 1)] void main(uint3 gl_GlobalInvocationID : SV_DispatchThreadID, uint3 gl_LocalInvocationID : SV_GroupThreadID, uint3 gl_WorkGroupID : SV_GroupID)/' \
      -e 's/uint\[3\](\(.*\));/{ \1 };/' $TA_SRC
}

# @1 a TA kernel: as ta_source_of() makes it, for Direct3D 11 (shader
# model 5.0, no root descriptors): the buffers declared at the start of a
# line as "StructuredBuffer<uint> " and "RWStructuredBuffer<uint> " are
# "Buffer<uint> " and "RWBuffer<uint> ", typed views of the same words,
# which read and write and take atomics as the structured ones do. And a
# compute shader of feature level 11.0 has eight unordered access views,
# u0 to u7: the two buffers past those, the per-pixel polygon words (u8)
# and the sorted indices (u9), are each read by one kernel alone
# (TAG_K_TRDRAW, TAG_K_PREPASS), which does not touch the modifier volume
# triangles (u1, which TAG_K_UNPACK alone writes): in those two kernels
# the two swap registers, so that the buffer is at u1, where the renderer
# binds it for that kernel (dx11/dx11_ta_decoder.cpp)
TA_K_TRDRAW=$(awk '$1 == "#define" && $2 == "TAG_K_TRDRAW" { print $3 }' $TA_SRC)
TA_K_PREPASS=$(awk '$1 == "#define" && $2 == "TAG_K_PREPASS" { print $3 }' $TA_SRC)
ta11_source_of() {
   a=; b=
   case $1 in
   $TA_K_TRDRAW) a=trp; b=u8 ;;
   $TA_K_PREPASS) a=srt; b=u9 ;;
   esac
   ta_source_of $1 | sed -e 's/^RWStructuredBuffer<uint> /RWBuffer<uint> /' -e 's/^StructuredBuffer<uint> /Buffer<uint> /' \
      -e "${a:+s/^\\(RWBuffer<uint> mtr_w : register(\\)u1);/\\1$b);/}" \
      -e "${a:+s/^\\(RWBuffer<uint> ${a}_w : register(\\)$b);/\\1u1);/}"
}

# @1 a compute kernel (PVRC_K_*), @2 the walk's samples a pixel, then
# lines to put in front (the test's): the shared rules' headers without
# their #includes, then pvr_compute.h, as kernels_spv.sh puts them for
# Vulkan, made HLSL of. The buffers' words "b.w[i]" are "b_w[i]";
# "layout(local_size_x = N) in;" is the kernel's PVRC_THREADS; a global
# variable (a line of declarations at the start of a line, with nothing
# but names and values) is "static"; a vector made of one value,
# "vec4(v)", is "(vec4)(v)", which HLSL makes it of; a 64-bit constant
# "Nl" is "int64_t(N)" (HLSL's "l" is not 64 bits); "x = a + atomicAdd(
# ...)" has the old value in a variable "x_was"
compute_source_of() {
   echo "#define PVR_SHADER_HLSL"; echo "#define PVRC_SHADER_HLSL"; echo "#define PVRC_KERNEL $1"
   if [ "$1" = "$PVRC_K_WALK" ]; then
      echo "#define PVRC_SAMPLES $2u"; echo "#define PVR_LANE_COUNT 8"; echo "#define PVR_LANE_GROUP_SIZE 256"
   else
      echo "#define PVR_LANE_NONE"
   fi
   shift 2
   sums=; depth=
   for d in "$@"; do
      echo "$d"
      [ "$d" = "#define PVRC_SUMS" ] && sums=1
      [ "$d" = "#define PVRC_DEPTH" ] && depth=1
   done
   # (and without the blocks for the HDR output, PVRC_SUMS, but in its
   # walks, nor those for the temporal upscaler's depth, PVRC_DEPTH, but in
   # its)
   printf '/^#include/d\n' > "$WORK/strip.sed"
   [ -n "$sums" ] || printf '/^#if defined(PVRC_SUMS)$/,/^#endif \\/\\* PVRC_SUMS \\*\\/$/d\n' >> "$WORK/strip.sed"
   [ -n "$depth" ] || printf '/^#if defined(PVRC_DEPTH)$/,/^#endif \\/\\* PVRC_DEPTH \\*\\/$/d\n' >> "$WORK/strip.sed"
   for h in core/rend/tile/pvr_lane.h core/rend/tile/pvr_tile.h core/rend/tile/pvr_modvol.h core/rend/tile/pvr_resolve.h core/rend/tile/pvr_scaler.h core/rend/ssaa_pattern.h $PVRC_SRC; do
      sed -f "$WORK/strip.sed" $h
   done | sed -e 's/\b\(frm\|rec\|bin\|vtx\|idx\|mvt\|dcmd\|dtrd\|dfrm\)\.w\[/\1_w[/g' \
      -e 's/^layout(local_size_x = \(.*\)) in;$/#define PVRC_THREADS \1/' \
      -e 's/^\(\(uint\|int\|bool\|float\|uvec4\|vec2\) [A-Za-z_0-9]*\( = [A-Za-z_0-9]*\)\?\(, [A-Za-z_0-9]*\( = [A-Za-z_0-9]*\)\?\)*;\)/static \1/' \
      -e 's/\b\(vec[234]\|ivec2\|uvec[24]\|i64vec3\)(\([^,()]*\))/((\1)(\2))/g' \
      -e 's/\b\(0x[0-9a-f]*\|[0-9][0-9]*\)l\b/int64_t(\1)/g' \
      -e 's/^\( *\)const uint \([a-z]*\) = \([a-z]*\) + atomicAdd(\(.*\), \([0-9a-z]*\));$/\1uint \2_was;\n\1InterlockedAdd(\4, \5, \2_was);\n\1const uint \2 = \3 + \2_was;/'
}

dxc_version() {
   if [ -n "$DXC" ] && [ -x "$DXC" ]; then "$DXC" --version 2>&1 | head -n 1; fi
}

# @1 the source, @2 the DXIL; the flags after
dxc_compile() {
   src=$1; out=$2; shift 2
   "$DXC" $DXC_FLAGS "$@" -Fo "$out" "$src" > "$WORK/log" 2>&1 || { cat "$WORK/log" >&2; exit 1; }
   # (signed: the container's digest, after its "DXBC", is not all zeros)
   [ "$(od -An -v -tx1 -j4 -N16 "$out" | tr -d ' \n')" != "00000000000000000000000000000000" ] \
      || { echo "$0: $DXC did not sign $out (no libdxil.so beside it?)" >&2; exit 1; }
}

compute_sums() {
   for k in $COMPUTE_KERNELS; do
      t=${k%:*}
      compute_source_of ${t%%:*} ${t#*:} "$(compute_defines_of $k | head -1)" "$(compute_defines_of $k | tail -1)" > "$WORK/s.hlsl"
      echo " * kernel $(compute_name_of $k) $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

# The compute renderer's root signature (dx12/dx12_compute.cpp's Init(), as
# it has it), which dxc holds each kernel's bindings to (-verifyrootsignature)
COMPUTE_ROOT="RootConstants(num32BitConstants=2, b0), DescriptorTable(UAV(u0, numDescriptors=10), UAV(u0, space=2, numDescriptors=3)), DescriptorTable(SRV(t0, space=1, numDescriptors=unbounded)), DescriptorTable(Sampler(s0, space=1, numDescriptors=unbounded)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, comparisonFunc=COMPARISON_NEVER)"
compute_root() {
   [ -f "$WORK/root.bin" ] && return
   echo "#define ROOT \"$COMPUTE_ROOT\"" > "$WORK/root.hlsl"
   "$DXC" -T rootsig_1_0 -E ROOT -Fo "$WORK/root.bin" "$WORK/root.hlsl" > "$WORK/log" 2>&1 || { cat "$WORK/log" >&2; exit 1; }
}

compute_hdr_sums() {
   for k in $COMPUTE_HDR_KERNELS; do
      t=${k%:*}
      compute_source_of ${t%%:*} ${t#*:} "$(compute_defines_of $k | head -1)" "$(compute_defines_of $k | tail -1)" "#define PVRC_SUMS" > "$WORK/s.hlsl"
      echo " * kernel $(compute_name_of $k) $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

# @1 a kernel of COMPUTE_UPSCALER_KERNELS: its source
upscaler_source_of() {
   upscaler_defines_of $1 > "$WORK/udefs"
   set --
   while read d; do set -- "$@" "$d"; done < "$WORK/udefs"
   compute_source_of $PVRC_K_WALK 1 "$@"
}

compute_upscaler_sums() {
   for k in $COMPUTE_UPSCALER_KERNELS; do
      upscaler_source_of $k > "$WORK/s.hlsl"
      echo " * kernel $(upscaler_name_of $k) $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

make_compute_upscaler_inc() {
   echo "/* DXIL of the compute renderer's walks for the temporal upscaler (rend/tile/"
   echo " * pvr_compute.h's walk with PVRC_DEPTH: each pixel's depth stored as well),"
   echo " * for 1 sample a pixel, in its two variants, and then the two that store"
   echo " * the channel sums too (PVRC_SUMS, for the HDR output), as"
   echo " * tools/rend/kernels_dx12.sh makes HLSL of them: made by"
   echo " * tools/rend/kernels_dx12.sh update-compute-upscaler from them. Not to be"
   echo " * edited."
   echo " * dxc $(dxc_version)"
   compute_upscaler_sums
   echo " */"
   for k in $COMPUTE_UPSCALER_KERNELS; do
      n=$(upscaler_name_of $k)
      upscaler_source_of $k > "$WORK/u$n.hlsl"
      compute_root
      dxc_compile "$WORK/u$n.hlsl" "$WORK/u$n.dxil"
      "$DXC" -dumpbin -verifyrootsignature "$WORK/root.bin" "$WORK/u$n.dxil" > "$WORK/log" 2>&1 \
         || { cat "$WORK/log" >&2; echo "$0: kernel $n binds what the compute renderer's root signature does not have" >&2; exit 1; }
      echo "static const u32 dx12_compute_upscaler_dxil_$n[] = {"
      od -An -v -w32 -tx4 --endian=little "$WORK/u$n.dxil" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// the sorted variant, the unsorted; then both with the sums: the words, and their size in bytes"
   l="static const u32 * const dx12_compute_upscaler_dxil[] = {"
   z="static const size_t dx12_compute_upscaler_dxil_size[] = {"
   c=
   for k in $COMPUTE_UPSCALER_KERNELS; do
      n=$(upscaler_name_of $k); l="$l$c dx12_compute_upscaler_dxil_$n"; z="$z$c sizeof(dx12_compute_upscaler_dxil_$n)"; c=,
   done
   echo "$l };"
   echo "$z };"
}

make_compute_hdr_inc() {
   echo "/* DXIL of the compute renderer's walks for the HDR output (rend/tile/"
   echo " * pvr_compute.h's walk with PVRC_SUMS: each pixel's channel sums stored"
   echo " * as well), for each of 1, 2, 4 and 8 samples a pixel, in its two"
   echo " * variants, as tools/rend/kernels_dx12.sh makes HLSL of them: made by"
   echo " * tools/rend/kernels_dx12.sh update-compute-hdr from them. Not to be"
   echo " * edited."
   echo " * dxc $(dxc_version)"
   compute_hdr_sums
   echo " */"
   for k in $COMPUTE_HDR_KERNELS; do
      n=$(compute_name_of $k)
      t=${k%:*}
      compute_source_of ${t%%:*} ${t#*:} "$(compute_defines_of $k | head -1)" "$(compute_defines_of $k | tail -1)" "#define PVRC_SUMS" > "$WORK/h$n.hlsl"
      compute_root
      dxc_compile "$WORK/h$n.hlsl" "$WORK/h$n.dxil"
      "$DXC" -dumpbin -verifyrootsignature "$WORK/root.bin" "$WORK/h$n.dxil" > "$WORK/log" 2>&1 \
         || { cat "$WORK/log" >&2; echo "$0: kernel $n binds what the compute renderer's root signature does not have" >&2; exit 1; }
      echo "static const u32 dx12_compute_hdr_dxil_$n[] = {"
      od -An -v -w32 -tx4 --endian=little "$WORK/h$n.dxil" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// the sorted variant by SSAA_LOG2() of its samples, then the unsorted: the words, and their size in bytes"
   l="static const u32 * const dx12_compute_hdr_dxil[] = {"
   z="static const size_t dx12_compute_hdr_dxil_size[] = {"
   c=
   for k in $COMPUTE_HDR_KERNELS; do
      n=$(compute_name_of $k); l="$l$c dx12_compute_hdr_dxil_$n"; z="$z$c sizeof(dx12_compute_hdr_dxil_$n)"; c=,
   done
   echo "$l };"
   echo "$z };"
}

# the kernels as words (DXIL's container is of whole words)
make_compute_inc() {
   echo "/* DXIL of the compute renderer's kernels (rend/tile/pvr_compute.h, each"
   echo " * after the shared rules' headers, with PVRC_SHADER_HLSL and its"
   echo " * PVRC_KERNEL, and the walk for each of 1, 2, 4 and 8 samples a pixel, in"
   echo " * its two variants (PVRC_VARIANT, the unsorted keeping one fragment), as"
   echo " * tools/rend/kernels_dx12.sh makes HLSL of them): made by"
   echo " * tools/rend/kernels_dx12.sh update from them. Not to be edited."
   echo " * dxc $(dxc_version)"
   compute_sums
   echo " */"
   for k in $COMPUTE_KERNELS; do
      n=$(compute_name_of $k)
      t=${k%:*}
      compute_source_of ${t%%:*} ${t#*:} "$(compute_defines_of $k | head -1)" "$(compute_defines_of $k | tail -1)" > "$WORK/c$n.hlsl"
      dxc_compile "$WORK/c$n.hlsl" "$WORK/c$n.dxil"
      echo "static const u32 dx12_compute_dxil_$n[] = {"
      od -An -v -w32 -tx4 --endian=little "$WORK/c$n.dxil" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by PVRC_K_* (the walk's sorted variant by SSAA_LOG2() of its samples, after the last, then its unsorted for 1, 2, 4 and 8): the words, and their size in bytes"
   l="static const u32 * const dx12_compute_dxil[] = { 0"
   z="static const size_t dx12_compute_dxil_size[] = { 0"
   for k in $COMPUTE_KERNELS; do
      case $k in *:0:0|*:1:1) n=$(compute_name_of $k); l="$l, dx12_compute_dxil_$n"; z="$z, sizeof(dx12_compute_dxil_$n)" ;; esac
   done
   for k in $COMPUTE_KERNELS; do
      case $k in *:2:1|*:4:1|*:8:1) n=$(compute_name_of $k); l="$l, dx12_compute_dxil_$n"; z="$z, sizeof(dx12_compute_dxil_$n)" ;; esac
   done
   for k in $COMPUTE_KERNELS; do
      case $k in *:2) n=$(compute_name_of $k); l="$l, dx12_compute_dxil_$n"; z="$z, sizeof(dx12_compute_dxil_$n)" ;; esac
   done
   echo "$l };"
   echo "$z };"
}

compiler_sum() {
   if [ -n "$D3DCOMPILER" ] && [ -f "$D3DCOMPILER" ]; then sha1sum < "$D3DCOMPILER" | cut -c1-40; fi
}

winpath() {
   case "$1" in /*) echo "Z:$1" ;; *) echo "$1" ;; esac
}

sums() {
   for k in $SHADERS; do
      n=${k%%:*}; r=${k#*:}; p=${r%%:*}; d=${r#*:}
      source_of $d > "$WORK/s.hlsl"
      echo " * shader $n $p $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

# @1 the source, @2 the profile, @3 the bytecode
compile() {
   [ -f "$WORK/dxbc_compile.exe" ] || $CC_WIN -O1 -o "$WORK/dxbc_compile.exe" tools/rend/dxbc_compile.c
   # (wine wants the TMPDIR it was set up with)
   env -u TMPDIR WINEDEBUG=-all $WINE "$WORK/dxbc_compile.exe" "$(winpath "$D3DCOMPILER")" "$(winpath "$1")" $2 \
      "$(winpath "$3")" > "$WORK/log" 2>&1 || { cat "$WORK/log" >&2; exit 1; }
}

ta_sums() {
   k=1
   while [ $k -le $TA_KERNELS ]; do
      ta_source_of $k > "$WORK/s.hlsl"
      echo " * kernel $k cs_5_1 $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
      k=$((k + 1))
   done
}

make_ta_inc() {
   echo "/* DXBC of GPU-side TA processing's kernels (rend/ta_gpu.h, each with"
   echo " * TAG_SHADER_HLSL and its TAG_KERNEL, as tools/rend/kernels_dx12.sh makes"
   echo " * HLSL of it): made by tools/rend/kernels_dx12.sh update from it. Not to"
   echo " * be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   ta_sums
   echo " */"
   k=1
   while [ $k -le $TA_KERNELS ]; do
      ta_source_of $k > "$WORK/t$k.hlsl"
      compile "$WORK/t$k.hlsl" cs_5_1 "$WORK/t$k.dxbc"
      echo "static const unsigned char dx12_ta_dxbc_$k[] = {"
      od -An -v -tx1 -w16 "$WORK/t$k.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
      k=$((k + 1))
   done
   echo "// by TAG_K_*: the bytes, and how many"
   l="static const unsigned char * const dx12_ta_dxbc[] = { 0"
   z="static const size_t dx12_ta_dxbc_size[] = { 0"
   i=1
   while [ $i -le $TA_KERNELS ]; do l="$l, dx12_ta_dxbc_$i"; z="$z, sizeof(dx12_ta_dxbc_$i)"; i=$((i + 1)); done
   echo "$l };"
   echo "$z };"
}

ta11_sums() {
   k=1
   while [ $k -le $TA_KERNELS ]; do
      ta11_source_of $k > "$WORK/s.hlsl"
      echo " * kernel $k cs_5_0 $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
      k=$((k + 1))
   done
}

make_ta11_inc() {
   echo "/* DXBC of GPU-side TA processing's kernels for Direct3D 11 (rend/ta_gpu.h,"
   echo " * each with TAG_SHADER_HLSL and its TAG_KERNEL, as tools/rend/kernels_dx12.sh"
   echo " * makes HLSL of it for Direct3D 11, shader model 5.0): made by"
   echo " * tools/rend/kernels_dx12.sh update from it. Not to be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   ta11_sums
   echo " */"
   k=1
   while [ $k -le $TA_KERNELS ]; do
      ta11_source_of $k > "$WORK/u$k.hlsl"
      compile "$WORK/u$k.hlsl" cs_5_0 "$WORK/u$k.dxbc"
      echo "static const unsigned char dx11_ta_dxbc_$k[] = {"
      od -An -v -tx1 -w16 "$WORK/u$k.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
      k=$((k + 1))
   done
   echo "// by TAG_K_*: the bytes, and how many"
   l="static const unsigned char * const dx11_ta_dxbc[] = { 0"
   z="static const size_t dx11_ta_dxbc_size[] = { 0"
   i=1
   while [ $i -le $TA_KERNELS ]; do l="$l, dx11_ta_dxbc_$i"; z="$z, sizeof(dx11_ta_dxbc_$i)"; i=$((i + 1)); done
   echo "$l };"
   echo "$z };"
}

# @1 the file, @2 what makes it, @3 what sums its sources, @4 the compiler
# (d3dcompiler_47 or dxc): whether it is theirs
check_inc() {
   $3 > "$WORK/sums"
   grep '^ \* \(shader\|kernel\) ' $1 > "$WORK/had" || true
   if ! cmp -s "$WORK/sums" "$WORK/had"; then
      echo "FAIL: $1 is not the bytecode of its sources as they are: run '$0 update'" >&2
      exit 1
   fi
   if [ "$4" = dxc ]; then c=$(dxc_version); else c=$(compiler_sum); fi
   if [ -n "$c" ] && grep -qF " * $4 $c" $1; then
      $2 > "$WORK/new.inc"
      if ! cmp -s "$WORK/new.inc" $1; then
         echo "FAIL: $1 is not what that $4 makes of its sources: run '$0 update'" >&2
         exit 1
      fi
      echo "kernels_dx12: $1 is its sources' bytecode (sums and bytes)"
   else
      echo "kernels_dx12: $1 is its sources' bytecode (sums; the bytes not compared: no compiler given, or another one)"
   fi
}

make_inc() {
   echo "/* DXBC of the Direct3D 12 renderer's shaders (rend/dx12/shaders/dx12_raster.hlsl,"
   echo " * each with its defines and rend/dx12/dx12_shader_flags.h): made by"
   echo " * tools/rend/kernels_dx12.sh update from them. Not to be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   sums
   echo " */"
   for k in $SHADERS; do
      n=${k%%:*}; r=${k#*:}; p=${r%%:*}; d=${r#*:}
      source_of $d > "$WORK/k$n.hlsl"
      compile "$WORK/k$n.hlsl" $p "$WORK/k$n.dxbc"
      echo "static const unsigned char dx12_dxbc_$n[] = {"
      od -An -v -tx1 -w16 "$WORK/k$n.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by DX12Shader: the bytes, and how many"
   l="static const unsigned char * const dx12_dxbc[] = { 0"
   z="static const size_t dx12_dxbc_size[] = { 0"
   i=1
   while [ $i -le $COUNT ]; do l="$l, dx12_dxbc_$i"; z="$z, sizeof(dx12_dxbc_$i)"; i=$((i + 1)); done
   echo "$l };"
   echo "$z };"
}

hdr_sums() {
   for k in $HDR_SHADERS; do
      n=${k%%:*}; r=${k#*:}; p=${r%%:*}; d=${r#*:}
      source_of $d > "$WORK/s.hlsl"
      echo " * shader $n $p $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

make_hdr_inc() {
   echo "/* DXBC of the Direct3D 12 renderer's shaders for the HDR output"
   echo " * (rend/dx12/shaders/dx12_raster.hlsl, each with its defines and"
   echo " * rend/dx12/dx12_shader_flags.h): made by tools/rend/kernels_dx12.sh"
   echo " * update-hdr from them. Not to be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   hdr_sums
   echo " */"
   for k in $HDR_SHADERS; do
      n=${k%%:*}; r=${k#*:}; p=${r%%:*}; d=${r#*:}
      source_of $d > "$WORK/h$n.hlsl"
      compile "$WORK/h$n.hlsl" $p "$WORK/h$n.dxbc"
      echo "static const unsigned char dx12_hdr_dxbc_$n[] = {"
      od -An -v -tx1 -w16 "$WORK/h$n.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by DX12HdrShader: the bytes, and how many"
   l="static const unsigned char * const dx12_hdr_dxbc[] = { 0"
   z="static const size_t dx12_hdr_dxbc_size[] = { 0"
   i=1
   while [ $i -le $HDR_COUNT ]; do l="$l, dx12_hdr_dxbc_$i"; z="$z, sizeof(dx12_hdr_dxbc_$i)"; i=$((i + 1)); done
   echo "$l };"
   echo "$z };"
}

case "$1" in
update-compute-upscaler)
   [ -n "$(dxc_version)" ] || { echo "$0: DXC=/path/to/dxc is needed to make them" >&2; exit 1; }
   make_compute_upscaler_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $COMPUTE_UPSCALER_INC
   echo "made $COMPUTE_UPSCALER_INC"
   ;;
update-compute-hdr)
   [ -n "$(dxc_version)" ] || { echo "$0: DXC=/path/to/dxc is needed to make them" >&2; exit 1; }
   make_compute_hdr_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $COMPUTE_HDR_INC
   echo "made $COMPUTE_HDR_INC"
   ;;
update-hdr)
   [ -n "$(compiler_sum)" ] || { echo "$0: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed to make them" >&2; exit 1; }
   make_hdr_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $HDR_INC
   echo "made $HDR_INC"
   ;;
update)
   [ -n "$(compiler_sum)" ] || { echo "$0: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed to make them" >&2; exit 1; }
   [ -n "$(dxc_version)" ] || { echo "$0: DXC=/path/to/dxc is needed to make them" >&2; exit 1; }
   make_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $INC
   make_ta_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $TA_INC
   make_ta11_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $TA11_INC
   make_compute_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $COMPUTE_INC
   make_hdr_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $HDR_INC
   make_compute_hdr_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $COMPUTE_HDR_INC
   make_compute_upscaler_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $COMPUTE_UPSCALER_INC
   echo "made $INC, $TA_INC, $TA11_INC, $COMPUTE_INC, $HDR_INC, $COMPUTE_HDR_INC and $COMPUTE_UPSCALER_INC"
   ;;
check)
   check_inc $INC make_inc sums d3dcompiler_47
   check_inc $TA_INC make_ta_inc ta_sums d3dcompiler_47
   check_inc $TA11_INC make_ta11_inc ta11_sums d3dcompiler_47
   check_inc $COMPUTE_INC make_compute_inc compute_sums dxc
   check_inc $HDR_INC make_hdr_inc hdr_sums d3dcompiler_47
   check_inc $COMPUTE_HDR_INC make_compute_hdr_inc compute_hdr_sums dxc
   check_inc $COMPUTE_UPSCALER_INC make_compute_upscaler_inc compute_upscaler_sums dxc
   ;;
bindings)
   ${CC:-cc} -std=c99 -O1 -Wall -DPVRC_KERNELS=$PVRC_KERNELS -DPVRC_K_WALK=$PVRC_K_WALK -Icore/rend/dx12 \
      -o "$WORK/dxil_bindings" tools/rend/dxil_bindings.c
   "$WORK/dxil_bindings"
   ;;
compute-test)
   [ -n "$(dxc_version)" ] || { echo "$0: DXC=/path/to/dxc is needed to make them" >&2; exit 1; }
   [ $# -eq 4 ] || { echo "usage: $0 compute-test DIR SAMPLES KEEP" >&2; exit 2; }
   k=1
   while [ $k -le "$PVRC_KERNELS" ]; do
      out="$2/k$k.dxil"
      [ $k = "$PVRC_K_WALK" ] && out="$2/k${k}_$3_$4.dxil"
      if [ -n "$TILE_PROD" ]; then t=0; else t=1; fi
      if [ ! -f "$out" ]; then
         v=0; [ $k = "$PVRC_K_WALK" ] && v=1
         compute_source_of $k $3 "#define PVRC_KEEP $4u" "#define PVRC_TEST ${t}u" "#define PVRC_EXACT" "#define PVRC_VARIANT ${v}u" > "$WORK/t.hlsl"
         dxc_compile "$WORK/t.hlsl" "$out"
      fi
      out="$2/k${k}u_$3.dxil"
      if [ $k = "$PVRC_K_WALK" ] && [ ! -f "$out" ]; then
         compute_source_of $k $3 "#define PVRC_KEEP 1u" "#define PVRC_TEST ${t}u" "#define PVRC_EXACT" "#define PVRC_VARIANT 2u" > "$WORK/t.hlsl"
         dxc_compile "$WORK/t.hlsl" "$out"
      fi
      k=$((k + 1))
   done
   ;;
*)
   echo "usage: $0 update | update-hdr | update-compute-hdr | update-compute-upscaler | check | bindings | compute-test DIR SAMPLES KEEP" >&2
   exit 2
   ;;
esac
