/* The HDR output (hdr_encode_vk.c): the picture the renderer drew,
 * through the Display Transform into the format the frontend took. After
 * hdr.glsl, with hdr_transform.glsl in it (beetle-psx's; kept as they are
 * there). Drawn over the whole picture by SSAA_RESOLVE_VERT's triangle:
 * with SUBPASS, as the last subpass of the frame's render pass, the
 * picture its input attachment; without, the picture sampled. */

#ifdef SUBPASS
layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput picture;
#else
layout (set = 0, binding = 0) uniform sampler2D picture;
#endif

layout (push_constant) uniform pushBlock
{
	vec4 toPanel[3];	/* linear Rec.2020 -> the panel's RGB, columns; zero when unknown */
	float paperWhite;
	float peak;
	float blackPq;
	int expandGamut;
	int shoulder;		/* the knees: 0 Reinhard, 1 exponential */
	int sdrEotf;
	int scrgb;
	int deband;		/* the picture is 8 bits a channel */
} pc;

layout (location = 0) out vec4 FragColor;

void main()
{
#ifdef SUBPASS
	highp vec3 rgb = subpassLoad(picture).rgb;
#else
	highp vec3 rgb = texelFetch(picture, ivec2(gl_FragCoord.xy), 0).rgb;
#endif
	if (pc.deband != 0)
		rgb = hdr_deband(rgb, gl_FragCoord.xy);
	FragColor = vec4(encode_hdr_output(rgb, pc.paperWhite, pc.peak, pc.expandGamut, pc.shoulder,
			pc.sdrEotf, 0, pc.blackPq, mat3(pc.toPanel[0].xyz, pc.toPanel[1].xyz, pc.toPanel[2].xyz),
			pc.scrgb), 1.0);
}
