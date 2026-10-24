#!/bin/sh
# The HDR output's encode in HLSL - what Direct3D 11 and 12 draw with,
# rend/hdr/hdr_encode.hlsl - against the GLSL the Vulkan renderer draws
# with, on one Vulkan device: the HLSL through glslang's HLSL front end,
# both over the same pictures, constants and tables, every Display
# Transform and setting (hdr_hlsl.c).
#
#   sh tools/vulkan/hdr_hlsl.sh
#
# Needs glslangValidator and a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json). No core.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/hdr_hlsl.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
# (the registers as the test's set has them: t0-t4 0-4, s0-s1 10-11, b0-b1 20-21)
glslangValidator -D -e main -S frag -V -DHDR_ENCODE_PS --stb 0 --ssb 10 --sub 20 \
   -o "$WORK/ps.spv" core/rend/hdr/hdr_encode.hlsl > "$WORK/log.txt" || { cat "$WORK/log.txt"; exit 1; }
glslangValidator -D -e main -S vert -V -DHDR_ENCODE_VS \
   -o "$WORK/vs.spv" core/rend/hdr/hdr_encode.hlsl > "$WORK/log.txt" || { cat "$WORK/log.txt"; exit 1; }
${CC:-cc} -O1 -g -Wall -I./core -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/hdr_hlsl" tools/vulkan/hdr_hlsl.c core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c \
   core/rend/hdr/hdr_aces2.c core/rend/hdr/hdr_display.c \
   core/libretro-common/rthreads/rthreads.c core/libretro-common/rthreads/retro_eventcount.c \
   -ldl -lpthread -lm
"$WORK/hdr_hlsl" "$WORK/ps.spv" ${HDR_HLSL_AGAINST:+--against "$HDR_HLSL_AGAINST"}
