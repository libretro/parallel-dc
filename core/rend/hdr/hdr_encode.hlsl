/* The HDR output's encode for Direct3D 11 and 12 (hdr_encode_d3d11.c,
 * hdr_encode_d3d12.c): rend/vulkan/shaders/hdr.glsl, hdr_transform.glsl
 * and hdr_encode.frag in HLSL, function for function and constant for
 * constant, so that the three backends draw the same picture. Shader model
 * 5.0, compiled once to DXBC for both (tools/rend/kernels_dxbc.sh:
 * hdr_encode_dxbc.inc): HDR_ENCODE_VS the triangle over the picture,
 * HDR_ENCODE_PS the encode. The HDR post effects' (hdr_output.h): with
 * HDR_ENCODE_LINEAR the encode's first pass into scene-linear light,
 * with HDR_ENCODE_POST the composite (hdr_post.frag), and HDR_BLOOM_PS
 * the bloom's stages (hdr_bloom.frag), one a define. tools/vulkan/hdr_hlsl.sh runs this through
 * glslang's HLSL front end on lavapipe against the GLSL.
 *
 * A GLSL mat3 is three columns, M * v their sum weighted by v; here it is
 * the struct mat3 and mmul(), so the matrices read as in the GLSL. */

#ifdef HDR_ENCODE_VS
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 p = float2((float)((id << 1) & 2), (float)(id & 2));
	return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef HDR_ENCODE_PS

struct mat3
{
	float3 c0;
	float3 c1;
	float3 c2;
};

mat3 m3(float a, float b, float c, float d, float e, float f, float g, float h, float i)
{
	mat3 m;
	m.c0 = float3(a, b, c);
	m.c1 = float3(d, e, f);
	m.c2 = float3(g, h, i);
	return m;
}

mat3 m3v(float4 c0, float4 c1, float4 c2)
{
	mat3 m;
	m.c0 = c0.xyz;
	m.c1 = c1.xyz;
	m.c2 = c2.xyz;
	return m;
}

float3 mmul(mat3 m, float3 v)
{
	return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z;
}

/* GLSL's inverse() of a mat3 */
mat3 minverse(mat3 m)
{
	float3 a = m.c0, b = m.c1, c = m.c2;
	float3 r0 = cross(b, c), r1 = cross(c, a), r2 = cross(a, b);
	float  inv = 1.0 / dot(a, r0);
	/* the rows of the inverse are r0, r1, r2: its columns are made of them */
	return m3(r0.x * inv, r1.x * inv, r2.x * inv,
	          r0.y * inv, r1.y * inv, r2.y * inv,
	          r0.z * inv, r1.z * inv, r2.z * inv);
}

/* GLSL's mod(): x - y * floor(x / y) */
float gmod(float x, float y)
{
	return x - y * floor(x / y);
}

/* hdr_transform.h's struct hdr_dt_block (std140), the GLSL's DisplayTransform */
cbuffer DisplayTransform : register(b0)
{
	float4 inRgbToCam[3];
	float4 inConeToAab[3];
	float4 limAabToCone[3];
	float4 limCamToRgb[3];
	float4 aces2ToOut[3];
	float4 dt_mode;
	float4 dt_sh;
	float4 dt_gt;
	float4 dt_film;
	float4 dt_inScalars;
	float4 dt_limScalars;
	float4 dt_ts;
	float4 dt_cc;
	float4 dt_cc2;
	float4 dt_gm;
	float4 dt_lutScale;
};

/* hdr_output.h's struct hdr_encode_params, hdr_encode.frag's push block */
cbuffer Params : register(b1)
{
	float4 toPanel[3];
	float  paperWhite;
	float  peak;
	float  blackPq;
	int    expandGamut;
	int    shoulder;
	int    sdrEotf;
	int    scrgb;
	int    deband;
	int    ssaaLog2;   /* HDR_ENCODE_SSAA: the samples', log2 */
	int    pad0;
	int    pad1;
	int    pad2;
};

Texture2D<float4> picture      : register(t0);
Texture2D<float4> uDtAces2Lut  : register(t1);
Texture2D<float4> uDtFilmLut   : register(t2);
Texture2D<float4> uDtFilmDesat : register(t3);
Texture3D<float4> uDtAces2Cube : register(t4);
#ifdef HDR_ENCODE_SSAA
/* With SSAA (after rend/tile/pvr_lane.h and pvr_resolve.h), the samples
 * the picture was resolved from, read again: where the picture is their
 * resolve (pvr_resolve_join(), the rule the resolve wrote it by) their
 * exact average is encoded, not the byte it was rounded to; where
 * something was drawn over the picture since (the VMUs, the crosshairs),
 * the picture. As hdr_encode.frag's with SSAA. */
Texture2DMS<float4> samples    : register(t5);

/* (a unorm8 colour's bytes, red lowest, as its picture holds them: exact) */
uint hdr_pack_unorm8(float4 v)
{
	const uint4 b = uint4(round(saturate(v) * 255.0));
	return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
#endif
#ifdef HDR_ENCODE_POST
/* the composite's: the scene-linear image (as the picture, t0), the
 * bloom's two bands, and struct hdr_post_params */
Texture2D<float4> bloomT       : register(t5);
Texture2D<float4> bloomW       : register(t6);
cbuffer Post : register(b2)
{
	float4 halBloom;   /* xyz the halation tint, w the bloom amount (0 off) */
	float2 bandW;      /* the tight band's weight, the wide one's */
	float  ca;         /* chromatic aberration, 0 off */
	float  postPad;
};
#endif
SamplerState      linearClamp  : register(s0);
SamplerState      linearWrap   : register(s1);

/* ---- hdr.glsl ---- */

static const float PQ_M1      = 2610.0 / 16384.0;
static const float PQ_M2      = (2523.0 / 4096.0) * 128.0;
static const float PQ_C1      = 3424.0 / 4096.0;
static const float PQ_C2      = (2413.0 / 4096.0) * 32.0;
static const float PQ_C3      = (2392.0 / 4096.0) * 32.0;
static const float PQ_MAXNITS = 10000.0;

float3 pq_encode(float3 nits)
{
	float3 y  = clamp(nits / PQ_MAXNITS, 0.0, 1.0);
	float3 ym = pow(y, PQ_M1);
	return pow((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), PQ_M2);
}

float3 pq_decode(float3 e)
{
	float3 p = pow(clamp(e, 0.0, 1.0), 1.0 / PQ_M2);
	return PQ_MAXNITS * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

float3 rec709_to_target(float3 c, int expand_gamut)
{
	if (expand_gamut == 1)
		return float3(
			 0.6274040 * c.r +  0.3292820 * c.g +  0.0433136 * c.b,
			 0.0457456 * c.r +  0.9417770 * c.g +  0.0124772 * c.b,
			-0.0012106 * c.r +  0.0176041 * c.g +  0.9836070 * c.b);
	else if (expand_gamut == 2)
		return float3(
			 0.8215873 * c.r +  0.1763479 * c.g +  0.0020641 * c.b,
			 0.0328261 * c.r +  0.9695096 * c.g + -0.0023367 * c.b,
			 0.0188038 * c.r +  0.0725063 * c.g +  0.9086907 * c.b);
	else if (expand_gamut == 3)
		return c;
	return float3(
		0.6274040 * c.r + 0.3292820 * c.g + 0.0433136 * c.b,
		0.0690970 * c.r + 0.9195400 * c.g + 0.0113612 * c.b,
		0.0163916 * c.r + 0.0880132 * c.g + 0.8955950 * c.b);
}

float3 sdr_to_linear(float3 c, int sdr_eotf)
{
	if (sdr_eotf == 1)
		return pow(c, 2.2);
	if (sdr_eotf == 2)
		return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
	return pow(c, 2.4);
}

float filmic_shoulder(float x)
{
	return 1.0 - exp(-x);
}

float3 panel_gamut_compress(float3 lin2020, mat3 to_panel)
{
	const float T = 0.9;
	const float S = 1.0 - T;
	float3 p;
	float  ach;
	float3 d;
	float3 x;
	if (to_panel.c0.x == 0.0)
		return lin2020;
	p   = mmul(to_panel, lin2020);
	ach = max(p.r, max(p.g, p.b));
	if (ach <= 0.0)
		return lin2020;
	d   = (ach - p) / ach;
	if (max(d.r, max(d.g, d.b)) <= T)
		return lin2020;
	x   = max(d - T, 0.0);
	d   = (d > T) ? (T + x / (1.0 + x / S)) : d;
	return mmul(minverse(to_panel), ach - d * ach);
}

float3 pq_black_floor(float3 e, float black_pq)
{
	float3 t;
	if (black_pq <= 0.0)
		return e;
	t = 1.0 - e;
	t = t * t;
	return e + black_pq * (t * t);
}

/* ---- hdr_transform.glsl ---- */

float dt_shoulder(float v, float base)
{
	float e;
	if (v <= 1.0)
		return base;
	e = v - 1.0;
	return 1.0 + e * dt_sh.x * dt_sh.y / (e + dt_sh.x);
}

float dt_cone_fwd(float v)
{
	float f = pow(abs(v), 0.42);
	float r = f / (27.13 + f);
	return v < 0.0 ? -r : r;
}

float dt_cone_inv(float v)
{
	float a = min(abs(v), 0.99);
	float r = pow((27.13 * a) / (1.0 - a), 1.0 / 0.42);
	return v < 0.0 ? -r : r;
}

float3 dt_rgb_to_jmh(float3 rgb, mat3 to_cam, mat3 cone_to_aab, float4 sc)
{
	float3 m   = mmul(to_cam, rgb);
	float3 aab = mmul(cone_to_aab, float3(dt_cone_fwd(m.x), dt_cone_fwd(m.y), dt_cone_fwd(m.z)));
	float  h;
	if (aab.x <= 0.0)
		return float3(0.0, 0.0, 0.0);
	h = degrees(atan2(aab.z, aab.y));
	if (h < 0.0)
		h += 360.0;
	return float3(100.0 * pow(aab.x, sc.y), length(aab.yz), h);
}

float3 dt_jmh_to_rgb(float3 jmh, mat3 aab_to_cone, mat3 to_rgb, float4 sc)
{
	float  hr  = radians(jmh.z);
	float3 aab = float3(pow(jmh.x * 0.01, sc.z), jmh.y * cos(hr), jmh.y * sin(hr));
	float3 a   = mmul(aab_to_cone, aab);
	return mmul(to_rgb, float3(dt_cone_inv(a.x), dt_cone_inv(a.y), dt_cone_inv(a.z)));
}

float dt_j_to_y(float J, float4 sc)
{
	return dt_cone_inv(sc.w * pow(abs(J) * 0.01, sc.z)) / sc.x;
}

float dt_y_to_j(float Y, float4 sc)
{
	float J = 100.0 * pow(dt_cone_fwd(abs(Y) * sc.x) / sc.w, sc.y);
	return Y < 0.0 ? -J : J;
}

float dt_ts_fwd(float x)
{
	float f = dt_ts.x * pow(max(x, 0.0) / (x + dt_ts.y), dt_ts.z);
	return max(f * f / (f + dt_ts.w), 0.0) * 100.0;
}

float dt_cc_norm(float h)
{
	float hr = radians(h);
	float a = cos(hr), b = sin(hr);
	float c2 = a * a - b * b, s2 = 2.0 * a * b;
	float c3 = 4.0 * a * a * a - 3.0 * a, s3 = 3.0 * b - 4.0 * b * b * b;
	return (11.34072 * a + 16.46899 * c2 + 7.88380 * c3 + 14.66441 * b
	        - 6.37224 * s2 + 9.19364 * s3 + 77.12896) * dt_cc2.y;
}

float dt_toe_fwd(float x, float limit, float k1in, float k2in)
{
	float k2, k1, k3, mb, mc;
	if (x > limit)
		return x;
	k2 = max(k2in, 0.001);
	k1 = sqrt(k1in * k1in + k2 * k2);
	k3 = (limit + k1) / (limit + k2);
	mb = k3 * x - k1;
	mc = k2 * k3 * x;
	return 0.5 * (mb + sqrt(mb * mb + 4.0 * mc));
}

float4 dt_lut_at(float h)
{
	float u = (gmod(h, 360.0) + 0.5) / 360.0;
	return uDtAces2Lut.SampleLevel(linearWrap, float2(u, 0.5), 0.0) * dt_lutScale;
}

float3 dt_chroma_compress(float3 jmh, float tmJ, float4 lut)
{
	float nJ, snJ, Mn, limit, M;
	if (jmh.y == 0.0)
		return float3(tmJ, 0.0, jmh.z);
	nJ    = tmJ / dt_cc.x;
	snJ   = max(0.0, 1.0 - nJ);
	Mn    = dt_cc_norm(jmh.z);
	limit = pow(nJ, dt_cc.y) * lut.z / Mn;
	M     = jmh.y * pow(tmJ / jmh.x, dt_cc.y) / Mn;
	M     = limit - dt_toe_fwd(limit - M, limit - 0.001, snJ * dt_cc.z, sqrt(nJ * nJ + dt_cc.w));
	M     = dt_toe_fwd(M, limit, nJ * dt_cc2.x, snJ);
	return float3(tmJ, M * Mn, jmh.z);
}

float dt_focus_gain(float J, float athr)
{
	float gain = dt_cc.x * dt_cc2.w;
	if (J > athr)
	{
		float adj = log(max(dt_cc.x - athr, 1e-6) / max(dt_cc.x - J, 0.0001)) / log(10.0);
		gain *= adj * adj + 1.0;
	}
	return gain;
}

float dt_solve_j(float J, float M, float fJ, float sg)
{
	float Ms = M / sg, a = Ms / fJ, b, c;
	if (J < fJ)
	{
		b = 1.0 - Ms;
		c = -J;
		return -2.0 * c / (b + sqrt(b * b - 4.0 * a * c));
	}
	b = -(1.0 + Ms + dt_cc.x * a);
	c = dt_cc.x * Ms + J;
	return -2.0 * c / (b - sqrt(b * b - 4.0 * a * c));
}

float dt_vec_slope(float iJ, float fJ, float sg)
{
	float d = (iJ < fJ) ? iJ : (dt_cc.x - iJ);
	return d * (iJ - fJ) / (fJ * sg);
}

float dt_smin(float a, float b, float ref)
{
	float s  = 0.12 * ref;
	float hh = max(s - abs(a - b), 0.0) / s;
	return min(a, b) - hh * hh * hh * s / 6.0;
}

float dt_bound_m(float iJ, float slope, float ig, float Jm, float Mm, float Jr)
{
	return Jr * pow(iJ / Jr, ig) * Mm / (Jm - slope * Mm);
}

float3 dt_gamut_compress(float3 jmh, float4 lut)
{
	float blend, fJ, athr, sg, iJ, slope, iJc, lower, upper, gb, rb, prop, thr, M;
	if (jmh.x <= 0.0)
		return float3(0.0, 0.0, jmh.z);
	if (jmh.y < 0.0 || jmh.x > dt_cc.x)
		return float3(jmh.x, 0.0, jmh.z);
	blend = min(1.3 - lut.x / dt_cc.x, 1.0);
	fJ    = lut.x + (dt_cc2.z - lut.x) * blend;
	athr  = lut.x + (dt_cc.x - lut.x) * 0.3;
	sg    = dt_focus_gain(jmh.x, athr);
	iJ    = dt_solve_j(jmh.x, jmh.y, fJ, sg);
	slope = dt_vec_slope(iJ, fJ, sg);
	iJc   = dt_solve_j(lut.x, lut.y, fJ, sg);
	lower = dt_bound_m(iJ, slope, dt_gm.x, lut.x, lut.y, iJc);
	upper = dt_bound_m(dt_cc.x - iJ, -slope, lut.w, dt_cc.x - lut.x, lut.y, dt_cc.x - iJc);
	gb    = dt_smin(lower, upper, lut.y);
	if (gb <= 0.0)
		return float3(jmh.x, 0.0, jmh.z);
	rb    = dt_bound_m(iJ, slope, dt_cc.y, dt_cc.x, lut.z, dt_cc.x);
	prop  = max(gb / rb, 0.75);
	thr   = prop * gb;
	M     = jmh.y;
	if (!(M <= thr || prop >= 1.0))
	{
		float mo = M - thr, go = gb - thr, ro = rb - thr;
		float scale = ro / ((ro / go) - 1.0);
		float nd = mo / scale;
		M = thr + scale * nd / (1.0 + nd);
	}
	return float3(iJ + M * slope, M, jmh.z);
}

float3 dt_clamp_ap0_to_ap1(float3 aces, float upper)
{
	mat3 to_ap1 = m3( 1.4514393161, -0.0765537733,  0.0083161484,
	                 -0.2365107469,  1.1762296998, -0.0060324498,
	                 -0.2149285693, -0.0996759265,  0.9977163014);
	mat3 to_ap0 = m3( 0.6954522414,  0.0447945634, -0.0055258826,
	                  0.1406786965,  0.8596711185,  0.0040252103,
	                  0.1638690622,  0.0955343182,  1.0015006723);
	return mmul(to_ap0, clamp(mmul(to_ap1, aces), 0.0, upper));
}

float3 dt_aces2(float3 v)
{
	mat3   to_ap0 = m3(0.4397010, 0.0897923, 0.0175440,
	                   0.3829780, 0.8134230, 0.1115440,
	                   0.1773350, 0.0967616, 0.8707040);
	float3 aces = dt_clamp_ap0_to_ap1(mmul(to_ap0, v) * dt_film.w, dt_gm.y);
	float3 jmh  = dt_rgb_to_jmh(aces, m3v(inRgbToCam[0], inRgbToCam[1], inRgbToCam[2]),
	                            m3v(inConeToAab[0], inConeToAab[1], inConeToAab[2]), dt_inScalars);
	float  tmJ  = dt_y_to_j(dt_ts_fwd(dt_j_to_y(jmh.x, dt_inScalars) * 0.01), dt_inScalars);
	float3 tm   = dt_chroma_compress(jmh, tmJ, dt_lut_at(jmh.z));
	float3 cg   = dt_gamut_compress(tm, dt_lut_at(tm.z));
	return dt_jmh_to_rgb(cg, m3v(limAabToCone[0], limAabToCone[1], limAabToCone[2]),
	                     m3v(limCamToRgb[0], limCamToRgb[1], limCamToRgb[2]), dt_limScalars);
}

float3 dt_aces_fit_rrt(float3 v)
{
	float3 a = v * (v + 0.0245786) - 0.000090537;
	float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
	return a / b;
}

float3 dt_curve_rgb(float3 c)
{
	int m = (int)(dt_mode.x + 0.5);
	if (m == 22)        /* AgX */
	{
		mat3 mi = m3(0.842479062253094, 0.0784335999999992, 0.0792237451477643,
		             0.0423282422610123, 0.878468636469772, 0.0791661274605434,
		             0.0423756549057051, 0.0784336, 0.879142973793104);
		mat3 mo = m3(1.19687900512017, -0.0980208811401368, -0.0990297440797205,
		             -0.0528968517574562, 1.15190312990417, -0.0989611768448433,
		             -0.0529716355144438, -0.0980434501171241, 1.15107367264116);
		float3 v = mmul(mi, max(c, 0.0));
		float3 v2, v4;
		v  = clamp((log2(max(v, 1e-10)) + 12.47393) / 16.500009, 0.0, 1.0);
		v2 = v * v;
		v4 = v2 * v2;
		v  = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4
		   - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
		v  = pow(max(mmul(mo, v), 0.0), 2.2) * 1.4491792;
		if (dt_sh.w > 0.5)  /* Punchy */
		{
			float3 u = pow(v, 1.35);
			float  n = dot(u, float3(0.2126, 0.7152, 0.0722));
			v = max(float3(n, n, n) + (u - n) * 1.4, 0.0);
		}
		return v;
	}
	if (m == 21)        /* Khronos PBR Neutral */
	{
		float  x    = min(c.r, min(c.g, c.b));
		float  off  = x < 0.08 ? x - 6.25 * x * x : 0.04;
		float3 v    = c - off;
		float  peakv = max(v.r, max(v.g, v.b));
		float  d, np, g;
		if (peakv < 0.76)
			return v * 1.1506276;
		d  = 1.0 - 0.76;
		np = 1.0 - d * d / (peakv + d - 0.76);
		v *= np / peakv;
		g  = 1.0 - 1.0 / (0.15 * (peakv - np) + 1.0);
		return lerp(v, float3(np, np, np), g) * 1.1506276;
	}
	if (m == 18)        /* Filmic Log (Sobotka) */
	{
		float3 t  = clamp((log2(max(c, 1e-10)) + 12.4739312) * 0.04, 0.0, 1.0);
		float3 d  = t * 32.0;
		float  bz = floor(d.z), fz = d.z - bz;
		float2 uv = float2((bz * 33.0 + min(d.x, 31.999) + 0.5) / 1089.0, (d.y + 0.5) / 33.0);
		float3 s0 = uDtFilmDesat.SampleLevel(linearClamp, uv, 0.0).rgb;
		float3 s1 = uDtFilmDesat.SampleLevel(linearClamp, uv + float2(33.0 / 1089.0, 0.0), 0.0).rgb;
		float3 o;
		t   = clamp(lerp(s0, s1, fz) * dt_film.x, 0.0, 1.0);
		o.r = uDtFilmLut.SampleLevel(linearClamp, float2(t.r, 0.5), 0.0).r;
		o.g = uDtFilmLut.SampleLevel(linearClamp, float2(t.g, 0.5), 0.0).r;
		o.b = uDtFilmLut.SampleLevel(linearClamp, float2(t.b, 0.5), 0.0).r;
		return pow(max(o, 0.0), 2.2) * dt_film.y;
	}
	if (m == 20)        /* ACES Fitted (Hill) */
	{
		mat3 mi = m3(0.59719, 0.07600, 0.02840,
		             0.35458, 0.90834, 0.13383,
		             0.04823, 0.01566, 0.83777);
		mat3 mo = m3( 1.60475, -0.10208, -0.00327,
		             -0.53108,  1.10813, -0.07276,
		             -0.07367, -0.00605,  1.07602);
		return max(mmul(mo, dt_aces_fit_rrt(mmul(mi, c))), 0.0) * 1.6152077;
	}
	{                   /* 19: Reinhard-Jodie */
		float  l  = dot(c, float3(0.2126, 0.7152, 0.0722));
		float3 tv = c / (1.0 + c);
		return lerp(c / (1.0 + l), tv, tv) * 2.0;
	}
}

float dt_curve(float v)
{
	int   m = (int)(dt_mode.x + 0.5);
	float x = max(v, 0.0);
	if (m == 1)
		return dt_shoulder(v, v * (2.51 * v + 0.03) / (v * (2.43 * v + 0.59) + 0.14) * 1.2440945);
	if (m == 2)
	{
		float h = max(v - 0.004, 0.0);
		return dt_shoulder(v, pow((h * (6.2 * h + 0.5)) / (h * (6.2 * h + 1.7) + 0.06), 2.2) * 1.4629683);
	}
	if (m == 3)
	{
		float t  = clamp(v / dt_gt.x, 0.0, 1.0);
		float w0 = 1.0 - (t * t * (3.0 - 2.0 * t));
		float w2 = step(dt_gt.y, v);
		float w1 = 1.0 - w0 - w2;
		float T  = dt_gt.x * pow(x / dt_gt.x, 1.33);
		float S  = 1.0 - dt_gt.z * exp(-dt_gt.w * (v - dt_gt.y));
		return (T * w0 + v * w1 + S * w2) * dt_sh.z;
	}
	if (m == 4)
		return dt_shoulder(v, (((v * (0.15 * v + 0.05) + 0.004) / (v * (0.15 * v + 0.50) + 0.06))
		                       - 0.0666667) * 4.5319149);
	if (m == 5)
		return dt_shoulder(v, (pow(x, 1.6) / (pow(x, 1.5632) * 0.1674199 + 1.0730397)) * 1.2404596);
	if (m == 6)
		return dt_shoulder(v, log(1.0 + x) / log(2.0 + 8.0 * pow(x / 8.0, 0.2344653))
		                      * (1.0 / 0.9542425) * 2.6616800);
	if (m == 7)
		return dt_shoulder(v, pow(v / (v + 0.155) * 1.019, 2.2) * 1.3173380);
	if (m == 8)
	{
		float va = 1.425 * x + 0.05;
		float f  = ((x * va + 0.004) / ((x * va + 0.055) + 0.0491)) - 0.0821;
		return dt_shoulder(v, pow(max(f, 0.0), 2.2) * 1.4132626);
	}
	if (m == 9)
		return dt_shoulder(v, (x / (1.0 + x)) * 2.0);
	if (m == 10)
		return dt_shoulder(v, (x * (1.0 + x / 16.0) / (1.0 + x)) * 1.8823529);
	if (m == 11)
		return dt_shoulder(v, (1.0 - exp(-x)) * 1.5819767);
	if (m == 12)
	{
		float f;
		if (x < 0.25)
			f = 1.1313708 * pow(x, 1.25);
		else if (x < 0.70)
			f = 0.20 + (x - 0.25);
		else
			f = 0.65 + 0.35 * (x - 0.70) / ((x - 0.70) + 0.35);
		return dt_shoulder(v, f * 1.2322275);
	}
	if (m == 13)
	{
		float f = 1.0471038 * pow(x / (x + 0.9198583), 1.15);
		return dt_shoulder(v, (f * f / (f + 0.04)) * 2.1854781);
	}
	if (m == 14)
		return dt_shoulder(v, pow(x, 0.6075));
	if (m == 15)
		return dt_shoulder(v, x);
	if (m == 16)
		return dt_shoulder(v, 2.3 * x / (2.3 * x - x + 1.0));
	return dt_shoulder(v, (x / (x + 0.3010864)) * 1.3010864);
}

float3 dt_expand_curve(float3 v, float E, float K)
{
	float3 t  = clamp((v - K) / max(1.0 - K, 1e-4), 0.0, 1.0);
	float3 hi = K + (1.0 - K) * t + (E - 1.0) * t * t + max(v - 1.0, 0.0);
	return lerp(v, hi, step(float3(K, K, K), v));
}

float3 dt_expand(float3 c)
{
	float E = dt_mode.y, p;
	if (dt_mode.w < 0.5 || E <= 1.0001)
		return c;
	if (dt_mode.w < 1.5)
		return dt_expand_curve(c, E, dt_film.z);
	p = max(c.r, max(c.g, c.b));
	if (p <= 1e-5)
		return c;
	return c * (dt_expand_curve(float3(p, p, p), E, dt_film.z).x / p);
}

float3 dt_aces2_cube(float3 v)
{
	const float n = 65.0;
	float3 y = clamp(v * dt_film.w * 0.01, 0.0, 1.0);
	float3 p = pow(y, 0.1593017578125);
	float3 u = pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
	return uDtAces2Cube.SampleLevel(linearClamp, u * ((n - 1.0) / n) + 0.5 / n, 0.0).rgb;
}

float3 dt_apply(float3 v, float paper_white_nits, out bool absolute)
{
	float pre;
	int   m = (int)(dt_mode.x + 0.5);
	v = dt_expand(max(v, 0.0));
	absolute = (m == 23);
	if (absolute)
		return mmul(m3v(aces2ToOut[0], aces2ToOut[1], aces2ToOut[2]),
		            dt_gm.z > 0.5 ? dt_aces2_cube(v) : dt_aces2(v)) * 100.0;
	pre = max(v.r, max(v.g, v.b));
	if (m >= 18)
		v = dt_curve_rgb(v);
	else
		v = float3(dt_curve(v.r), dt_curve(v.g), dt_curve(v.b));
	if (dt_mode.z > 0.5)
	{
		float post = max(v.r, max(v.g, v.b));
		float g    = 1.0 - 1.0 / (0.15 * max(pre - post, 0.0) + 1.0);
		v = lerp(v, float3(post, post, post), g);
	}
	return v;
}

/* ---- hdr.glsl's grade and encode ---- */

float3 hdr_grade_linear(float3 scene_linear, float paper_white_nits, float peak_nits,
                        int expand_gamut, int shoulder_mode, mat3 to_panel)
{
	float  headroom, omax, o, knee;
	float3 lin, over;
	if (dt_mode.x > 0.5)
	{
		bool   absolute;
		float3 v = dt_apply(max(scene_linear, 0.0), paper_white_nits, absolute);
		if (absolute)
			return v;
		return panel_gamut_compress(rec709_to_target(v * paper_white_nits, expand_gamut), to_panel);
	}
	headroom = max(peak_nits - paper_white_nits, 0.0);
	lin      = max(scene_linear, 0.0) * paper_white_nits;
	over     = (headroom > 0.0) ? max(lin - paper_white_nits, 0.0) : 0.0;
	omax     = max(over.r, max(over.g, over.b));
	o        = (omax > 0.0) ? (omax / headroom) : 0.0;
	knee     = 0.0;
	if (o > 0.0)
	{
		if (shoulder_mode == 1)
			knee = (o < 1e-2)
			          ? (1.0 - o * (0.5 - o * (1.0 / 6.0)))
			          : (filmic_shoulder(o) / o);
		else
			knee = 1.0 / (o + 1.0);
	}
	lin = min(lin, paper_white_nits) + over * knee;
	lin = rec709_to_target(lin, expand_gamut);
	return panel_gamut_compress(lin, to_panel);
}

float3 rec2020_to_709(float3 c)
{
	return float3(
		 1.6604903 * c.r + -0.5876391 * c.g + -0.0728516 * c.b,
		-0.1245500 * c.r +  1.1328999 * c.g + -0.0083480 * c.b,
		-0.0181511 * c.r + -0.1005787 * c.g +  1.1187299 * c.b);
}

float3 encode_hdr_output(float3 rgb, float paper_white_nits, float peak_nits,
                         int expand_gamut, int shoulder_mode, int sdr_eotf,
                         float black_pq, mat3 to_panel, int scrgb_out)
{
	float3 lin = hdr_grade_linear(sdr_to_linear(max(rgb, 0.0), sdr_eotf),
	                              paper_white_nits, peak_nits, expand_gamut, shoulder_mode, to_panel);
	if (scrgb_out == 0)
		return pq_black_floor(pq_encode(lin), black_pq);
	if (black_pq > 0.0)
		lin = pq_decode(pq_black_floor(pq_encode(lin), black_pq));
	return rec2020_to_709(lin) * (1.0 / 80.0);
}

float hdr_ign(float2 p, float2 k)
{
	return frac(52.9829189 * frac(dot(p, k)));
}

float hdr_tri(float u)
{
	return u < 0.5 ? sqrt(2.0 * u) - 1.0 : 1.0 - sqrt(2.0 - 2.0 * u);
}

float3 hdr_deband(float3 rgb, float2 fragcoord)
{
	float3 t = float3(
		hdr_tri(hdr_ign(fragcoord, float2( 0.06711056,  0.00583715))),
		hdr_tri(hdr_ign(fragcoord, float2( 0.00583715,  0.06711056))),
		hdr_tri(hdr_ign(fragcoord, float2( 0.06711056, -0.00583715))));
	return rgb + t * (1.0 / 255.0);
}

/* ---- hdr_encode.frag ---- */

#ifdef HDR_ENCODE_POST
/* ---- hdr_post.frag ---- */

float3 encode_hdr_output_linear(float3 scene_linear, float paper_white_nits, float peak_nits,
                                int expand_gamut, int shoulder_mode, float black_pq,
                                mat3 to_panel, int scrgb_out)
{
	float3 lin = hdr_grade_linear(scene_linear, paper_white_nits, peak_nits,
	                              expand_gamut, shoulder_mode, to_panel);
	if (scrgb_out == 0)
		return pq_black_floor(pq_encode(lin), black_pq);
	if (black_pq > 0.0)
		lin = pq_decode(pq_black_floor(pq_encode(lin), black_pq));
	return rec2020_to_709(lin) * (1.0 / 80.0);
}

float4 main(float4 pos : SV_Position) : SV_Target
{
	uint w, h;
	picture.GetDimensions(w, h);
	/* (the scene image is the output's size) */
	const float2 vUV = pos.xy / float2((float)w, (float)h);
	float3 lin = picture.SampleLevel(linearClamp, vUV, 0.0).rgb;
	if (ca != 0.0)
	{
		const float2 d = vUV - 0.5;
		const float  r = dot(d, d) * ca;
		lin.r = picture.SampleLevel(linearClamp, vUV - d * r, 0.0).r;
		lin.b = picture.SampleLevel(linearClamp, vUV + d * r, 0.0).b;
	}
	if (halBloom.w > 0.0)
	{
		float3 bl = bandW.x * bloomT.SampleLevel(linearClamp, vUV, 0.0).rgb;
		if (bandW.y > 0.0)
			bl += bandW.y * bloomW.SampleLevel(linearClamp, vUV, 0.0).rgb;
		lin += (halBloom.w * halBloom.xyz) * bl;
	}
	return float4(encode_hdr_output_linear(max(lin, 0.0), paperWhite, peak, expandGamut, shoulder,
			blackPq, m3v(toPanel[0], toPanel[1], toPanel[2]), scrgb), 1.0);
}
#else
float4 main(float4 pos : SV_Position) : SV_Target
{
	const float4 pic = picture.Load(int3((int)pos.x, (int)pos.y, 0));
	float3 rgb = pic.rgb;
#ifdef HDR_ENCODE_SSAA
	{
		uint rb = 0u;
		uint ga = 0u;
		for (uint s = 0u; s < (1u << (uint)ssaaLog2); s++)
		{
			const uint c = hdr_pack_unorm8(samples.Load(int2(pos.xy), s));
			rb += pvr_resolve_rb(c);
			ga += pvr_resolve_ga(c);
		}
		if (hdr_pack_unorm8(pic) == pvr_resolve_join(rb, ga, (uint)ssaaLog2))
			rgb = float3((float)(rb & 0xffffu), (float)(ga & 0xffffu), (float)(rb >> 16))
					/ (255.0 * (float)(1u << (uint)ssaaLog2));
	}
#endif
	if (deband != 0)
		rgb = hdr_deband(rgb, pos.xy);
#ifdef HDR_ENCODE_LINEAR
	return float4(sdr_to_linear(max(rgb, 0.0), sdrEotf), 1.0);
#else
	return float4(encode_hdr_output(rgb, paperWhite, peak, expandGamut, shoulder, sdrEotf,
			blackPq, m3v(toPanel[0], toPanel[1], toPanel[2]), scrgb), 1.0);
#endif
}
#endif

#endif

#ifdef HDR_BLOOM_PS
/* ---- hdr_bloom.frag: one stage a define (struct hdr_bloom_params) ---- */

Texture2D<float4> source      : register(t0);
SamplerState      linearClamp : register(s0);
cbuffer Bloom : register(b0)
{
	float4 p0;
	float4 p1;   /* xy the step in the source, zw the target's texel */
};

static float2 vUV;

float3 tap(float2 o)
{
	return source.SampleLevel(linearClamp, vUV + p1.xy * o, 0.0).rgb;
}

#ifdef HDR_BLOOM_DOWN_KARIS
float kw(float3 c)
{
	return 1.0 / (1.0 + max(c.r, max(c.g, c.b)));
}
#endif

float4 main(float4 pos : SV_Position) : SV_Target
{
	vUV = pos.xy * p1.zw;
#if defined(HDR_BLOOM_BRIGHT)
	float3 lin = 0.25 * (tap(float2(-1.0, -1.0)) + tap(float2(1.0, -1.0)) +
	                     tap(float2(-1.0, 1.0)) + tap(float2(1.0, 1.0)));
	lin = max(lin, 0.0);
	const float lum = dot(lin, float3(0.2126, 0.7152, 0.0722));
	float sk = clamp(lum - p0.x + p0.y, 0.0, 2.0 * p0.y);
	sk = sk * sk / (4.0 * p0.y);
	const float3 b = lin * (max(sk, lum - p0.x) / max(lum, 1e-5)) / (1.0 - p0.x);
	const float l = dot(b, float3(0.2126, 0.7152, 0.0722));
	return float4(b / (1.0 + l * p0.z), 1.0);
#elif defined(HDR_BLOOM_BLUR)
	float3 c = tap(float2(0.0, 0.0)) * 0.24458;
	c += tap(float2( 1.36268,  1.36268)) * 0.31802;
	c += tap(float2(-1.36268, -1.36268)) * 0.31802;
	c += tap(float2( 3.21159,  3.21159)) * 0.05717;
	c += tap(float2(-3.21159, -3.21159)) * 0.05717;
	c += tap(float2( 5.11234,  5.11234)) * 0.00251;
	c += tap(float2(-5.11234, -5.11234)) * 0.00251;
	return float4(c, 1.0);
#elif defined(HDR_BLOOM_DOWN) || defined(HDR_BLOOM_DOWN_KARIS)
	const float3 a = tap(float2(-2.0,  2.0));
	const float3 b = tap(float2( 0.0,  2.0));
	const float3 c = tap(float2( 2.0,  2.0));
	const float3 d = tap(float2(-2.0,  0.0));
	const float3 e = tap(float2( 0.0,  0.0));
	const float3 f = tap(float2( 2.0,  0.0));
	const float3 g = tap(float2(-2.0, -2.0));
	const float3 h = tap(float2( 0.0, -2.0));
	const float3 i = tap(float2( 2.0, -2.0));
	const float3 j = tap(float2(-1.0,  1.0));
	const float3 k = tap(float2( 1.0,  1.0));
	const float3 l = tap(float2(-1.0, -1.0));
	const float3 m = tap(float2( 1.0, -1.0));
#ifdef HDR_BLOOM_DOWN_KARIS
	float3 acc = 0.0;
	float  ws  = 0.0;
	float  w;
	w = kw(a) * 0.03125; acc += a * w; ws += w;
	w = kw(b) * 0.06250; acc += b * w; ws += w;
	w = kw(c) * 0.03125; acc += c * w; ws += w;
	w = kw(d) * 0.06250; acc += d * w; ws += w;
	w = kw(e) * 0.12500; acc += e * w; ws += w;
	w = kw(f) * 0.06250; acc += f * w; ws += w;
	w = kw(g) * 0.03125; acc += g * w; ws += w;
	w = kw(h) * 0.06250; acc += h * w; ws += w;
	w = kw(i) * 0.03125; acc += i * w; ws += w;
	w = kw(j) * 0.12500; acc += j * w; ws += w;
	w = kw(k) * 0.12500; acc += k * w; ws += w;
	w = kw(l) * 0.12500; acc += l * w; ws += w;
	w = kw(m) * 0.12500; acc += m * w; ws += w;
	return float4(acc / ws, 1.0);
#else
	float3 o = e * 0.125;
	o += (a + c + g + i) * 0.03125;
	o += (b + d + f + h) * 0.0625;
	o += (j + k + l + m) * 0.125;
	return float4(o, 1.0);
#endif
#else /* HDR_BLOOM_UP */
	const float r = p0.x;
	float3 s = tap(float2(-r, -r));
	s += tap(float2(0.0, -r)) * 2.0;
	s += tap(float2( r, -r));
	s += tap(float2(-r, 0.0)) * 2.0;
	s += tap(float2(0.0, 0.0)) * 4.0;
	s += tap(float2( r, 0.0)) * 2.0;
	s += tap(float2(-r,  r));
	s += tap(float2(0.0,  r)) * 2.0;
	s += tap(float2( r,  r));
	return float4(s * 0.0625, 1.0);
#endif
}
#endif
