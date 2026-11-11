/* The Direct3D 11 write-out pass's HLSL, alone so the shaders made ahead
 * (dx11_shaders.cpp, tools/rend/dx11_shaders_ahead_test.sh) take it
 * without the pass: see dx11_writeout.h. The flags it reads are checked
 * against pvr_compute.h's in dx11_writeout.cpp. */
#include "dx11_shaders.h"

/* The pass: a thread a column and a remainder of the lines mod m, which
 * walks down its lines in place, as pvr_compute.h's PVRC_K_YFILTER does */
const char *const DX11WriteOutSource =
"#define PVR_SHADER_HLSL\n#define PVR_LANE_NONE\n"
#include "rend/tile/pvr_resolve_text.inc"
#include "rend/tile/pvr_scaler_text.inc"
R"(
RWTexture2D<unorm float4> picture : register(u0);
cbuffer WriteOut : register(b0)
{
   uint4 words;	// the width, the height, the frame's PVRC_F_FLAGS
};

uint wo_load(int2 p)
{
   uint4 v = uint4(round(picture[p] * 255.0));
   return v.x | (v.y << 8) | (v.z << 16) | (v.w << 24);
}

[numthreads(64, 1, 1)]
void main(uint3 global : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
   uint w = words.x, h = words.y, flags = words.z;
   uint m = (flags >> 3) & 31u, ycoeff = flags >> 16, fbw = (flags >> 8) & 63u;
   int x = int(global.x);
   uint y = group.y;
   if (m == 0u || uint(x) >= w || y >= h)
      return;
   uint mid = wo_load(int2(x, int(y)));
   uint above = mid;
   for (; y < h; y += m)
   {
      uint below = y + m < h ? wo_load(int2(x, int(y + m))) : mid;
      uint c = pvr_fb_write((flags & 0x4000u) != 0u ? pvr_scaler_y(above, mid, below, ycoeff) : mid, uint(x) / m, y / m, fbw);
      picture[int2(x, int(y))] = (float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) + 0.25) / 255.0;
      above = mid;
      mid = below;
   }
}
)";
