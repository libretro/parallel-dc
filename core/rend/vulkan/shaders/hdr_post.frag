/* The HDR post effects' composite (hdr_encode_vk.c), beetle-psx's
 * hdr_post.frag (after boom3's hdr_composite): chromatic aberration, the
 * (halation-tinted) bloom, then the grade and encode hdr_encode.frag would
 * have run. Reads the scene-linear image its LINEAR_OUT pass wrote (1.0 =
 * paper white) and the two bloom bands. After hdr.glsl, with
 * hdr_transform.glsl in it. */

layout (set = 0, binding = 0) uniform sampler2D uScene;
layout (set = 0, binding = 13) uniform sampler2D uBloomT;
layout (set = 0, binding = 14) uniform sampler2D uBloomW;

/* hdr_output.h's struct hdr_encode_params, then struct hdr_post_params */
layout (push_constant) uniform pushBlock
{
	vec4 toPanel[3];
	float paperWhite;
	float peak;
	float blackPq;
	int expandGamut;
	int shoulder;
	int sdrEotf;
	int scrgb;
	int deband;
	int ssaaLog2;
	int pad0;
	int pad1;
	int pad2;
	vec4 halBloom;		/* xyz the halation tint, w the bloom amount (0 off) */
	vec2 bandW;		/* the tight band's weight, the wide one's */
	float ca;		/* chromatic aberration, 0 off */
	float pad3;
} pc;

layout (location = 0) out vec4 FragColor;

void main()
{
	/* (the scene image is the output's size) */
	highp vec2 vUV = gl_FragCoord.xy / vec2(textureSize(uScene, 0));
	highp vec3 lin = textureLod(uScene, vUV, 0.0).rgb;

	/* Transverse chromatic aberration: a lens focuses long and short
	 * wavelengths at slightly different magnifications, so red and blue
	 * land at slightly different distances from the axis. Grows with the
	 * square of the field height: nothing at the centre, most at the
	 * corners; a fixed fraction of the frame, so the same at every
	 * internal resolution. */
	if (pc.ca != 0.0)
	{
		highp vec2  d = vUV - 0.5;
		highp float r = dot(d, d) * pc.ca;
		lin.r = textureLod(uScene, vUV - d * r, 0.0).r;
		lin.b = textureLod(uScene, vUV + d * r, 0.0).b;
	}

	/* The bloom in, before the roll-off, so the glow lands in the
	 * headroom; the wide band fetched only where it is weighted.
	 * Halation tints it toward red-orange, the way light scattered back
	 * off a film's base comes back reddened. */
	if (pc.halBloom.w > 0.0)
	{
		highp vec3 bl = pc.bandW.x * textureLod(uBloomT, vUV, 0.0).rgb;
		if (pc.bandW.y > 0.0)
			bl += pc.bandW.y * textureLod(uBloomW, vUV, 0.0).rgb;
		lin += (pc.halBloom.w * pc.halBloom.xyz) * bl;
	}

	FragColor = vec4(encode_hdr_output_linear(max(lin, vec3(0.0)), pc.paperWhite, pc.peak,
			pc.expandGamut, pc.shoulder, 0, pc.blackPq,
			mat3(pc.toPanel[0].xyz, pc.toPanel[1].xyz, pc.toPanel[2].xyz), pc.scrgb), 1.0);
}
