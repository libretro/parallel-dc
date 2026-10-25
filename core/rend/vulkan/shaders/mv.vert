// The motion vector pass of the temporal upscaler (rend/upscaler.h),
// after position.glsl: a vertex placed as the colour pass placed it, so
// the depth test against the depth that pass left finds the same
// surface; the vector from where it is to where it was last frame, in
// pixels of the render (the jitter is in both, and cancels).

layout (constant_id = 10) const float VIEWPORT_WIDTH = 640.0;
layout (constant_id = 11) const float VIEWPORT_HEIGHT = 480.0;

layout (location = 0) in vec4 in_pos;
layout (location = 1) in vec2 in_prev;

layout (location = 0) out vec2 motion;

void main()
{
	vec4 vpos = in_pos;
	if (vpos.z < 0.0 || vpos.z > 3.4e37)
	{
		gl_Position = vec4(0.0, 0.0, 1.0, 1.0 / vpos.z);
		motion = vec2(0.0);
		return;
	}
	gl_Position = pvr_position(vpos);
	// (the game's coordinates are on the screen already: the matrix only scales and moves them)
	vec2 now = (uniformBuffer.normal_matrix * vpos).xy;
	vec2 then = (uniformBuffer.normal_matrix * vec4(in_prev, vpos.z, 1.0)).xy;
	motion = (then - now) * 0.5 * vec2(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
}
