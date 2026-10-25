// The motion vector pass (mv.vert) over a depth the compute renderer
// stored (rend/tile/pvr_compute.h, PVRC_DEPTH_ON): the depth test
// against it here, the surface the frame left or nearer (reversed),
// with a little room, as the kernel's depth and the rasterizer's are
// not the same to the bit.

layout (set = 0, binding = 1) uniform sampler2D depthTex;

layout (location = 0) in vec2 motion;

layout (location = 0) out vec2 fragmentColor;

void main()
{
	float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
	if (gl_FragCoord.z < d - 1e-5)
		discard;
	fragmentColor = motion;
}
