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

#ifdef SSAA
/* With SSAA (after pso_key.h and pvr_resolve.h), the samples the picture
 * was resolved from, read again: where the picture is their resolve
 * (pvr_resolve_join(), the rule the resolve wrote it by) their exact
 * average is encoded, not the byte it was rounded to; where something was
 * drawn over the picture since (the VMUs, the crosshairs), the picture. */
layout (constant_id = PSO_SC_SSAA_SAMPLES) const int SSAA_SAMPLES = 4;
layout (constant_id = PSO_SC_SSAA_LOG2N) const uint SSAA_LOG2N = 2u;
#ifdef SUBPASS
layout (input_attachment_index = 1, set = 0, binding = 13) uniform subpassInputMS samples;
#else
/* (the per-pixel and compute renderers': no samples by the time the
 * picture is encoded, but their channel sums, stored by the last pass
 * (oit_final.frag) or the walk (pvr_compute.h), scaled to the constants'
 * 16 samples - hdr_encode_vk.h's HDR_VK_SUMS_SAMPLES) */
layout (set = 0, binding = 13) uniform usampler2D sums;
#endif
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
	int ssaaLog2;		/* (Direct3D 12's; here a constant: SSAA_LOG2N) */
	int pad0;
	int pad1;
	int pad2;
} pc;

layout (location = 0) out vec4 FragColor;

void main()
{
#ifdef SUBPASS
	highp vec4 pic = subpassLoad(picture);
#else
	highp vec4 pic = texelFetch(picture, ivec2(gl_FragCoord.xy), 0);
#endif
	highp vec3 rgb = pic.rgb;
#ifdef SSAA
#ifdef SUBPASS
	uint rb = 0u;
	uint ga = 0u;
	for (int s = 0; s < SSAA_SAMPLES; s++)
	{
		uint c = packUnorm4x8(subpassLoad(samples, s));
		rb += pvr_resolve_rb(c);
		ga += pvr_resolve_ga(c);
	}
#else
	uvec4 t = texelFetch(sums, ivec2(gl_FragCoord.xy), 0);
	uint rb = t.r | (t.b << 16);
	uint ga = t.g | (t.a << 16);
#endif
	if (packUnorm4x8(pic) == pvr_resolve_join(rb, ga, SSAA_LOG2N))
		rgb = vec3(float(rb & 0xffffu), float(ga & 0xffffu), float(rb >> 16)) / (255.0 * float(SSAA_SAMPLES));
#endif
	if (pc.deband != 0)
		rgb = hdr_deband(rgb, gl_FragCoord.xy);
	FragColor = vec4(encode_hdr_output(rgb, pc.paperWhite, pc.peak, pc.expandGamut, pc.shoulder,
			pc.sdrEotf, 0, pc.blackPq, mat3(pc.toPanel[0].xyz, pc.toPanel[1].xyz, pc.toPanel[2].xyz),
			pc.scrgb), 1.0);
}
