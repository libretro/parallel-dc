#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
#include "gl4.h"
#include "rend/tileclip.h"
#include "rend/gles/ta_decoder_gl.h"
#include "rend/gles/glprogram_cache.h"
#include "rend/modvol_clip.h"

/*

Drawing and related state management
Takes vertex, textures and renders to the currently set up target
*/

static gl4PipelineShader* CurrentShader;
GLuint geom_fbo;
GLuint stencilTexId;
GLuint opaqueTexId;
GLuint depthTexId;
GLuint texSamplers[2];
GLuint depth_fbo;
GLuint depthSaveTexId;

/* (tests: every polygon drawn with the general programs, to see they draw
 * what the polygons' own ones do) */
#ifdef GL_PROGRAM_TEST_GENERAL
static const bool PipelineTestGeneral = true;
#else
static const bool PipelineTestGeneral = false;
#endif

static void pipeline_from_key(gl4PipelineShader *s, u32 key)
{
	s->key = key;
	s->pp_InsideClipping = (key >> 16) & 1;
	s->cp_AlphaTest = (key >> 15) & 1;
	s->pp_Texture = (key >> 14) & 1;
	s->pp_UseAlpha = (key >> 13) & 1;
	s->pp_IgnoreTexA = (key >> 12) & 1;
	s->pp_ShadInstr = (key >> 10) & 3;
	s->pp_Offset = (key >> 9) & 1;
	s->pp_FogCtrl = (key >> 7) & 3;
	s->pp_TwoVolumes = (key >> 6) & 1;
	s->pp_Gouraud = (key >> 5) & 1;
	s->pp_BumpMap = (key >> 4) & 1;
	s->fog_clamping = (key >> 3) & 1;
	s->palette = (key >> 2) & 1;
	s->pass = (int)(key & 3);
}

/* A polygon drawn with the general program @g until its own is there */
static void use_general(gl4PipelineShader *s, gl4PipelineShader *g)
{
	const u32 key = s->key;
	const GLuint pending = s->pending;
	gl4PipelineShader bits = *s;
	*s = *g;
	/* (its own bits, the general one's program and uniforms) */
	s->cp_AlphaTest = bits.cp_AlphaTest;
	s->pp_InsideClipping = bits.pp_InsideClipping;
	s->pp_Texture = bits.pp_Texture;
	s->pp_UseAlpha = bits.pp_UseAlpha;
	s->pp_IgnoreTexA = bits.pp_IgnoreTexA;
	s->pp_ShadInstr = bits.pp_ShadInstr;
	s->pp_Offset = bits.pp_Offset;
	s->pp_FogCtrl = bits.pp_FogCtrl;
	s->pass = bits.pass;
	s->pp_TwoVolumes = bits.pp_TwoVolumes;
	s->pp_Gouraud = bits.pp_Gouraud;
	s->pp_BumpMap = bits.pp_BumpMap;
	s->fog_clamping = bits.fog_clamping;
	s->palette = bits.palette;
	s->key = key;
	s->pending = pending;
	s->general = g;
}

/* A key first drawn (in a frame): drawn with the general program for the
 * rest of the run, with no compile or link here (AGENTS.md); the key is
 * kept, and its own program made at the start of the next. Without a
 * general program (it did not link), made now as ever. */
static void pipeline_first(gl4PipelineShader *s, u32 key)
{
	gl4PipelineShader *g;
	pipeline_from_key(s, key);
	glprog_note(GLPROG_GL4, key);
	g = &gl4.general[s->pass][s->pp_Gouraud];
	if (g->program == 0)
	{
		gl4CompilePipelineShader(s, gl4PixelPipelineShader, NULL);
		return;
	}
	use_general(s, g);
	glprog_general_used();
}

static gl4PipelineShader *gl4GetProgram(bool cp_AlphaTest, bool pp_InsideClipping,
							bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
							u32 pp_FogCtrl, bool pp_TwoVolumes, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping,
							bool palette, int pass)
{
	gl4PipelineShader *shader;
	u32 rv=0;

	rv |= pp_InsideClipping;
	rv <<= 1; rv |= cp_AlphaTest;
	rv <<= 1; rv |= pp_Texture;
	rv <<= 1; rv |= pp_UseAlpha;
	rv <<= 1; rv |= pp_IgnoreTexA;
	rv <<= 2; rv |= pp_ShadInstr;
	rv <<= 1; rv |= pp_Offset;
	rv <<= 2; rv |= pp_FogCtrl;
	rv <<= 1; rv |= pp_TwoVolumes;
	rv <<= 1; rv |= pp_Gouraud;
	rv <<= 1; rv |= pp_BumpMap;
	rv <<= 1; rv |= fog_clamping;
	rv <<= 1; rv |= palette;
	rv <<= 2; rv |= (u32)pass;

	shader = (gl4PipelineShader *)tex_map_find(&gl4.shaders, rv);
	if (shader == NULL && (shader = (gl4PipelineShader *)tex_map_add(&gl4.shaders, rv)) == NULL)
		die("Out of memory");
	if (shader->program == 0)
		pipeline_first(shader, rv);

	return shader;
}

/* The keys more than one game draws with (glprogram_cache.h), of Jet Grind
 * Radio, Dead or Alive 2, Sega Rally 2 and Virtua Fighter 4: made at the
 * start with those of earlier runs. (The general programs draw the
 * others the first time.) */
static const u32 CommonKeys[] = {
	0x0121, 0x1521, 0x2100, 0x3100, 0x5101, 0x6f22,
};

/* (the keys made at the start, at most) */
#define GL4_START_KEYS 1024

void gl4MakePrograms(void)
{
	/* (the renderer's thread's, at its start) */
	static u32 keys[GL4_START_KEYS];
	static u32 failed[1024];
	u32 nkeys = 0, count = 0, nfailed = 0, i, e;
	const u32 *known;
	u64 key;
	int p, g;

	if (gl4.shaders.elem == 0)
		tex_map_init(&gl4.shaders, sizeof(gl4PipelineShader), NULL, NULL);
	gl4.shaders.name = "OpenGL programs";
	tex_map_reserve(&gl4.shaders, 2048);
	for (p = 0; p < 3; p++)
		for (g = 0; g < 2; g++)
		{
			gl4PipelineShader *s = &gl4.general[p][g];
			memset(s, 0, sizeof(*s));
			s->pp_Gouraud = g;
			s->pass = p;
			s->program = gl4MakePipelineProgram(s, true, true);
		}
	known = glprog_known(GLPROG_GL4, &count);
	for (i = 0; i < count; i++)
		if (known[i] <= 0x1FFFF && (known[i] & 3) != 3 && nkeys < GL4_START_KEYS)
			keys[nkeys++] = known[i];
	for (i = 0; i < sizeof(CommonKeys) / sizeof(CommonKeys[0]) && nkeys < GL4_START_KEYS; i++)
		keys[nkeys++] = CommonKeys[i];
	if (!PipelineTestGeneral)
		for (i = 0; i < nkeys; i++)
		{
			gl4PipelineShader *s = (gl4PipelineShader *)tex_map_find(&gl4.shaders, keys[i]);
			if (s == NULL && (s = (gl4PipelineShader *)tex_map_add(&gl4.shaders, keys[i])) == NULL)
				die("Out of memory");
			if (s->program != 0 || s->pending != 0)
				continue;
			pipeline_from_key(s, keys[i]);
			s->pending = gl4MakePipelineProgram(s, false, true);
		}
	/* now waited for */
	for (p = 0; p < 3; p++)
		for (g = 0; g < 2; g++)
		{
			gl4PipelineShader *s = &gl4.general[p][g];
			if (glprog_wait(s->program))
				gl4PipelineLocations(s, true);
			else
			{
				WARN_LOG(RENDERER, "OpenGL: no general program (pass %d, pp_Gouraud %d)\n", p, g);
				glDeleteProgram(s->program);
				s->program = 0;
			}
		}
	/* (those that did not link go: taken away after, not while they are
	 * gone through) */
	for (e = 0; e < tex_map_places(&gl4.shaders); e++)
	{
		gl4PipelineShader *s = (gl4PipelineShader *)tex_map_at(&gl4.shaders, e, &key);
		if (s != NULL && s->pending != 0 && s->program == 0)
		{
			const GLuint prog = s->pending;
			s->pending = 0;
			if (glprog_wait(prog))
			{
				s->program = prog;
				gl4PipelineLocations(s, false);
			}
			else
			{
				WARN_LOG(RENDERER, "OpenGL: the program of key %x did not link\n", s->key);
				glDeleteProgram(prog);
				if (nfailed < sizeof(failed) / sizeof(failed[0]))
					failed[nfailed++] = (u32)key;
			}
		}
	}
	for (i = 0; i < nfailed; i++)
		tex_map_remove(&gl4.shaders, failed[i]);
}

/* What the two samplers' parameters were last set to: each set again only
 * when a polygon wants another value (a sampler earlier draws still use
 * that is changed has the driver keep a copy of it). Forgotten when they
 * are made again. */
enum { SAMP_WRAP_S, SAMP_WRAP_T, SAMP_MIN, SAMP_MAG, SAMP_LOD_BIAS, SAMP_ANISO, SAMP_COUNT };
static u32 samplerSet[2];
static u32 samplerValue[2][SAMP_COUNT];

static void samplerInt(int index, int which, GLenum pname, GLint value)
{
	if ((samplerSet[index] & (1u << which)) && samplerValue[index][which] == (u32)value)
		return;
	samplerSet[index] |= 1u << which;
	samplerValue[index][which] = (u32)value;
	glSamplerParameteri(texSamplers[index], pname, value);
}

static void samplerFloat(int index, int which, GLenum pname, float value)
{
	u32 bits;
	memcpy(&bits, &value, 4);
	if ((samplerSet[index] & (1u << which)) && samplerValue[index][which] == bits)
		return;
	samplerSet[index] |= 1u << which;
	samplerValue[index][which] = bits;
	glSamplerParameterf(texSamplers[index], pname, value);
}

static void SetTextureRepeatMode(int index, GLuint dir, u32 clamp, u32 mirror)
{
	samplerInt(index, dir == GL_TEXTURE_WRAP_S ? SAMP_WRAP_S : SAMP_WRAP_T, dir,
			clamp ? GL_CLAMP_TO_EDGE : mirror ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

static void SetBaseClipping(void)
{
	if (gl4ShaderUniforms.base_clipping.enabled)
		gl4Scissor(gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y, gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
	else
		gl4ScissorOff();
}

/* (a polygon's control words, in C their u32s) */
#define PPW(w, f) TAW_GET(w, f)

/* A polygon's state, of list @Type, @SortingEnabled, for @pass (each call
 * gives them as constants: what does not apply to its list goes) */
static INLINE void SetGPState(u32 Type, bool SortingEnabled, int pass, const PolyParam* gp)
{
	int clip_rect[4] = { 0, 0, 0, 0 };
	int clipmode;
	bool palette = false;
	u32 stencil;

	/* Trilinear filtering. Ignore if texture isn't mipmapped (shenmue snowflakes) */
	if (PPW(gp->pcw, PCW_Texture) && PPW(gp->tsp, TSP_FilterMode) > 1 && Type != ListType_Punch_Through && PPW(gp->tcw, TCW_MipMapped) == 1)
	{
		gl4ShaderUniforms.trilinear_alpha = 0.25 * (PPW(gp->tsp, TSP_MipMapD) & 0x3);
		if (PPW(gp->tsp, TSP_FilterMode) == 2)
			/* Trilinear pass A */
			gl4ShaderUniforms.trilinear_alpha = 1.0 - gl4ShaderUniforms.trilinear_alpha;
	}
	else
		gl4ShaderUniforms.trilinear_alpha = 1.0;

	clipmode = tile_clip(gp->tileclip, &ViewportMatrix, clip_rect);

	if (pass == PASS_DEPTH)
	{
		/* A punch-through polygon is cut out by its texture's alpha in this
		 * pass too. Where the texture is one of palette indices, that alpha
		 * is in the palette: without it the cut-out was made from the
		 * indices, and what the polygon left in the depth and stencil
		 * buffers - shadows on it, what shows through it - was wrong.
		 * (Upstream: shadows in Alone in the Dark.) */
		palette = Type == ListType_Punch_Through && PPW(gp->pcw, PCW_Texture)
				&& tex_gpu_paletted(gp->tsp, gp->tcw, 0);
		CurrentShader = gl4GetProgram(Type == ListType_Punch_Through ? true : false,
				clipmode == TILECLIP_INSIDE,
				Type == ListType_Punch_Through ? PPW(gp->pcw, PCW_Texture) : false,
				true,
				PPW(gp->tsp, TSP_IgnoreTexA),
				0,
				false,
				2,
				false,	/* TODO Can PT have two different textures for area 0 and 1 ?? */
				false,
				false,
				false,
				palette,
				pass);
	}
	else
	{
		/* Two volumes mode only supported for OP and PT */
		const bool two_volumes_mode = (gp->tsp1 != (u32)-1) && Type != ListType_Translucent;
		const bool color_clamp = PPW(gp->tsp, TSP_ColorClamp) && (pvrrc.fog_clamp_min != 0 || pvrrc.fog_clamp_max != 0xffffffff);
		const int fog_ctrl = PPW(gp->tsp, TSP_FogCtrl);

		palette = tex_gpu_paletted(gp->tsp, gp->tcw, 0);

		CurrentShader = gl4GetProgram(Type == ListType_Punch_Through ? true : false,
				clipmode == TILECLIP_INSIDE,
				PPW(gp->pcw, PCW_Texture),
				PPW(gp->tsp, TSP_UseAlpha),
				PPW(gp->tsp, TSP_IgnoreTexA),
				PPW(gp->tsp, TSP_ShadInstr),
				PPW(gp->pcw, PCW_Offset),
				fog_ctrl,
				two_volumes_mode,
				PPW(gp->pcw, PCW_Gouraud),
				PPW(gp->tcw, TCW_PixelFmt) == TA_PIXEL_BUMPMAP,
				color_clamp,
				palette,
				pass);
	}
	glc_use_program(CurrentShader->program);

	if (palette)
	{
		if (PPW(gp->tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
			gl4ShaderUniforms.palette_index = ((float)(PPW(gp->tcw, TCW_PalSelect) << 4) + 0.5f) / 1024.f;
		else
			gl4ShaderUniforms.palette_index = ((float)((PPW(gp->tcw, TCW_PalSelect) >> 4) << 8) + 0.5f) / 1024.f;
	}

	gl4ShaderUniforms.tsp0 = gp->tsp;
	gl4ShaderUniforms.tsp1 = gp->tsp1;
	gl4ShaderUniforms.tcw0 = gp->tcw;
	gl4ShaderUniforms.tcw1 = gp->tcw1;
	gl4_uniforms_set(CurrentShader);

	/* An opaque polygon is blended by its instructions too when it is in a
	 * list continuation; the ones of the first render pass have had theirs
	 * set to source x 1 and destination x 0 when the list was read. That
	 * pair is no blending at all, and is left as that so it costs nothing. */
	if (pass == PASS_COLOR && (Type == ListType_Translucent || Type == ListType_Punch_Through
			|| PPW(gp->tsp, TSP_SrcInstr) != 1 || PPW(gp->tsp, TSP_DstInstr) != 0))
	{
		glc_enable(GL_BLEND);
		glc_blend_func(SrcBlendGL[PPW(gp->tsp, TSP_SrcInstr)], DstBlendGL[PPW(gp->tsp, TSP_DstInstr)]);
	}
	else
		glc_disable(GL_BLEND);

	if (clipmode == TILECLIP_INSIDE)
		glUniform4f(CurrentShader->pp_ClipTest, clip_rect[0], clip_rect[1], clip_rect[0] + clip_rect[2], clip_rect[1] + clip_rect[3]);
	if (clipmode == TILECLIP_OUTSIDE)
	{
		if (gl4ShaderUniforms.base_clipping.enabled)
			intersect_clip_rect(clip_rect, gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y,
					gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
		gl4Scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
	}
	else
		SetBaseClipping();

	/* This bit controls which pixels are affected by modvols */
	stencil = PPW(gp->pcw, PCW_Shadow) != 0 ? 0x80 : 0x0;

	glc_stencil_func(GL_ALWAYS, stencil, stencil);

	if (CurrentShader->pp_Texture)
	{
		int i;
		for (i = 0; i < 2; i++)
		{
			const GLuint texid = (GLuint)(i == 0 ? gp->texid : gp->texid1);

			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, texid == (GLuint)-1 ? 0 : texid);

			if (texid != (GLuint)-1)
			{
				const u32 tsp = i == 0 ? gp->tsp : gp->tsp1;
				/* Mipmapping does not depend on the filter: see the
				 * per-triangle renderer. This went by the first texture's
				 * word for the second texture too. */
				const u32 tcw = i == 0 ? gp->tcw : gp->tcw1;
				const bool index_texture = i == 0 && palette;
				const u32 fmt = PPW(tcw, TCW_PixelFmt);
				const bool mipmapped = !index_texture && PPW(tcw, TCW_MipMapped) != 0 && !settings.rend.ForceTextureLOD0
						&& (PPW(tcw, TCW_ScanOrder) == 0 || fmt == TA_PIXEL_4BPP || fmt == TA_PIXEL_8BPP);

				glBindSampler(i, texSamplers[i]);
				SetTextureRepeatMode(i, GL_TEXTURE_WRAP_S, PPW(tsp, TSP_ClampU), PPW(tsp, TSP_FlipU));
				SetTextureRepeatMode(i, GL_TEXTURE_WRAP_T, PPW(tsp, TSP_ClampV), PPW(tsp, TSP_FlipV));

				/* the two samplers are shared by every texture: what one polygon set, the next has unless it is set again */
				samplerFloat(i, SAMP_LOD_BIAS, GL_TEXTURE_LOD_BIAS, mipmapped ? D_Adjust_LoD_Bias[PPW(tsp, TSP_MipMapD)] : 0.f);
				if (PPW(tsp, TSP_FilterMode) == 0)
				{
					/* No filtering - the level is still picked. And no
					 * anisotropy: left on from a filtered polygon, it had
					 * these filtered all the same, a texture of palette
					 * indices included. */
					samplerInt(i, SAMP_MIN, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
					samplerInt(i, SAMP_MAG, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					if (gl.max_anisotropy > 1.f)
						samplerFloat(i, SAMP_ANISO, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
				}
				else
				{
					/* bilinear filtering (the PowerVR has trilinear too, in
					 * two passes: not done here) */
					const bool trilinear = gl.max_anisotropy > 1.f && settings.rend.AnisotropicFiltering > 1
							&& mipmapped && Type != ListType_Punch_Through;
					/* Set the recommended minification filter for best results with anisotropy - not
					 * for a punch-through polygon, whose alpha two levels blended would spoil */
					samplerInt(i, SAMP_MIN, GL_TEXTURE_MIN_FILTER,
							trilinear ? GL_LINEAR_MIPMAP_LINEAR : mipmapped ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
					samplerInt(i, SAMP_MAG, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					if (gl.max_anisotropy > 1.f)
					{
						const f32 aniso = (f32)settings.rend.AnisotropicFiltering;
						samplerFloat(i, SAMP_ANISO, GL_TEXTURE_MAX_ANISOTROPY_EXT, settings.rend.AnisotropicFiltering > 1
								? (aniso < gl.max_anisotropy ? aniso : gl.max_anisotropy) : 1.f);
					}
				}
			}
		}
		glActiveTexture(GL_TEXTURE0);
	}

	/* The cull mode: gcflip the whole picture's, for renders to textures
	 * (their Y is mirrored) */
	SetCull(PPW(gp->isp, ISP_TSP_CullMode) ^ gcflip);

	/* the depth function, only where it is needed */
	if (Type == ListType_Punch_Through || (pass == PASS_DEPTH && SortingEnabled))
		glc_depth_func(GL_GEQUAL);
	else if (Type == ListType_Opaque || (Type == ListType_Translucent && !SortingEnabled))
		glc_depth_func(Zfunction[PPW(gp->isp, ISP_TSP_DepthMode)]);

	if (pass == PASS_DEPTH || pass == PASS_COLOR)
	{
		/* Z Write Disable seems to be ignored for punch-through polys
		 * (Worms World Party, Bust-a-Move 4 and Re-Volt) */
		if (Type == ListType_Punch_Through)
			glc_depth_mask(GL_TRUE);
		else
			glc_depth_mask(!PPW(gp->isp, ISP_TSP_ZWriteDis));
	}
	else
		glc_depth_mask(GL_FALSE);
}

static void DrawList(u32 Type, bool SortingEnabled, int pass, const PolyParam *head, int first, int count)
{
	const PolyParam *params = head + first;

	if (count == 0)
		return;

	while (count-- > 0)
	{
		if (params->count > 2)
		{
			if ((Type == ListType_Opaque || (Type == ListType_Translucent && !SortingEnabled)) && PPW(params->isp, ISP_TSP_DepthMode) == 0)
			{
				/* depthFunc = never */
				params++;
				continue;
			}
			if (pass == PASS_OIT && Type == ListType_Translucent && PPW(params->tsp, TSP_SrcInstr) == 0 && PPW(params->tsp, TSP_DstInstr) == 1)
			{
				/* dst = dst */
				params++;
				continue;
			}
			/* (of a frame decoded on the GPU, a draw group's translucent
			 * strips are numbered by their draws: TA_GPU_INSTANCE) */
			gl4ShaderUniforms.poly_number = pvrrc.gpu ? 0 : (int)(params - head);
			SetGPState(Type, SortingEnabled, pass, params);
			if (pvrrc.gpu)
				gltad_draw_group(Type, params->first, params->count, false);
			else
				glDrawElements(GL_TRIANGLE_STRIP, params->count, GL_UNSIGNED_INT, (GLvoid*)(sizeof(u32) * params->first));
			glCheck();
		}

		params++;
	}
}

void gl4SetupMainVBO(GLuint geometry, GLuint idxs)
{
	glBindVertexArray(gl4.vbo.main_vao);

	glBindBuffer(GL_ARRAY_BUFFER, geometry != 0 ? geometry : gl4.vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, idxs != 0 ? idxs : gl4.vbo.idxs);

	/* setup vertex buffers attrib pointers */
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));

	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col));

	glEnableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,vtx_spc));

	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u));

	glEnableVertexAttribArray(VERTEX_COL_BASE1_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_COL_BASE1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, col1)); glCheck();

	glEnableVertexAttribArray(VERTEX_COL_OFFS1_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_COL_OFFS1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, spc1)); glCheck();

	glEnableVertexAttribArray(VERTEX_UV1_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_UV1_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u1)); glCheck();
}

/* What the vertex arrays and storage binding 1 have: frame_buffers -1 not
 * known, -2 the GPU decode's, or the vertex buffer of the renderer's own
 * or of the frame's they have; and the volumes' triangles' buffer */
static long frame_buffers = -1;
static GLuint frame_modvols;

void gl4ForgetFrameBuffers(void)
{
	frame_buffers = -1;
}

void gl4BindFrameBuffers(bool gpu, bool own)
{
	const GlTaDraw *d;
	const GLuint geometry = own ? gl4.vbo.geometry : gl_frame_vertex_buffer(gl4.vbo.geometry);
	const GLuint modvols = own ? gl4.vbo.modvols : gl_frame_modvol_buffer(gl4.vbo.modvols);
	const long want = gpu ? -2 : (long)geometry;
	if (frame_buffers == want && (gpu || frame_modvols == modvols))
		return;
	frame_buffers = want;
	frame_modvols = modvols;
	if (!gpu)
	{
		gl4SetupMainVBO(geometry, own ? gl4.vbo.idxs : gl_frame_index_buffer(gl4.vbo.idxs));
		glDisableVertexAttribArray(VERTEX_PP_ARRAY);
		/* (an attribute without its array: the number is 0) */
		gl4.vertexAttribI4ui(VERTEX_PP_ARRAY, 0, 0, 0, 0);
		gl4SetupModvolVBO(modvols);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl4.vbo.tr_poly_params);
		glBindVertexArray(gl4.vbo.main_vao);
		return;
	}
	d = gltad_draw();
	glBindVertexArray(gl4.vbo.main_vao);
	gltad_bind_geometry();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col));
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,vtx_spc));
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u));
	glVertexAttribPointer(VERTEX_COL_BASE1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, col1));
	glVertexAttribPointer(VERTEX_COL_OFFS1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, spc1));
	glVertexAttribPointer(VERTEX_UV1_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u1));
	/* a strip's number: one a draw, the draw record's baseInstance picking it */
	glBindBuffer(GL_ARRAY_BUFFER, d->instances);
	glVertexAttribIPointer(VERTEX_PP_ARRAY, 1, GL_UNSIGNED_INT, sizeof(u32), (void*)0);
	gl4.vertexAttribDivisor(VERTEX_PP_ARRAY, 1);
	glEnableVertexAttribArray(VERTEX_PP_ARRAY);
	glBindVertexArray(gl4.vbo.modvol_vao);
	gltad_bind_modvols();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void*)0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, d->trParams);
	glBindVertexArray(gl4.vbo.main_vao);
}

void gl4SetupModvolVBO(GLuint modvols)
{
	glBindVertexArray(gl4.vbo.modvol_vao);

	glBindBuffer(GL_ARRAY_BUFFER, modvols != 0 ? modvols : gl4.vbo.modvols);

	/* setup vertex buffers attrib pointers */
	glEnableVertexAttribArray(VERTEX_POS_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float)*3, (void*)0); glCheck();
}

static void DrawModVols(int first, int count)
{
	const ModifierVolumeParam *params;
	int mod_base = -1;
	u32 clip = 0;	/* what the scissor is set for: so far nothing, the whole picture */
	int cmv;

	/* A bit of explanation:
	* In theory it works like this: generate a 1-bit stencil for each polygon
	* volume, and then AND or OR it against the overall 1-bit tile stencil at
	* the end of the volume. */

	if (count == 0)
		return;

	glBindVertexArray(gl4.vbo.modvol_vao);

	glc_disable(GL_BLEND);
	SetBaseClipping();

	glc_use_program(gl4.modvol_shader.program);

	glc_enable(GL_DEPTH_TEST);
	glc_depth_mask(GL_FALSE);
	glc_depth_func(GL_GREATER);

	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	params = &TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam)[first];

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode, cull;
		mv_clip_range_get(&pvrrc, TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam), param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = TAW_GET(param->isp, ISP_Modvol_DepthMode);
		cull = TAW_GET(param->isp, ISP_Modvol_CullMode);

		/* A volume is clipped like a polygon, to the rectangle it was sent
		 * under if it asked to be: its shadow fell outside it. (Kept in:
		 * the scissor. Kept out is not done, as it is not for upstream.) */
		if (param->tileclip != clip)
		{
			int clip_rect[4];
			clip = param->tileclip;
			if (tile_clip(clip, &ViewportMatrix, clip_rect) == TILECLIP_OUTSIDE)
				gl4Scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
			else
				SetBaseClipping();
		}

		if (mod_base == -1)
			mod_base = mvFirst;

		if (!TAW_GET(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
			gl_set_mvs_mode(Or, cull);		/* OR'ing (open volume or quad) */
		else
			gl_set_mvs_mode(Xor, cull);	/* XOR'ing (closed volume) */

		glDrawArrays(GL_TRIANGLES, mvFirst * 3, mvCount * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			gl_set_mvs_mode(mv_mode == 1 ? Inclusion : Exclusion, cull);
			glDrawArrays(GL_TRIANGLES, mod_base * 3, (mvFirst + mvCount - mod_base) * 3);
			mod_base = -1;
		}
	}
	SetBaseClipping();
	/* restore states */
	glBindVertexArray(gl4.vbo.main_vao);
	glc_enable(GL_DEPTH_TEST);
	glc_depth_mask(GL_TRUE);
}


static GLuint CreateColorFBOTexture(int width, int height)
{
	GLuint texId = glc_gen_texture();
	glc_bind_texture(GL_TEXTURE_2D, texId);
	glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
	glCheck();

	return texId;
}

/* The working buffer's colour is the screen's last picture: see gl4DrawStrips(). */
static bool working_holds_screen;

void gl4CreateTextures(int width, int height)
{
	GLuint uStatus;
	working_holds_screen = false;
	if (geom_fbo == 0)
	{
		glGenFramebuffers(1, &geom_fbo);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);

	stencilTexId = glc_gen_texture();
	glBindTexture(GL_TEXTURE_2D, stencilTexId); glCheck();
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);		/* OpenGL >= 4.3 */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	/* Using glTexStorage2D instead of glTexImage2D to satisfy requirement GL_TEXTURE_IMMUTABLE_FORMAT=true, needed for glTextureView below */
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, width, height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencilTexId, 0); glCheck();

	opaqueTexId = CreateColorFBOTexture(width, height);

	depthTexId = glc_gen_texture();
	glTextureView(depthTexId, GL_TEXTURE_2D, stencilTexId, GL_DEPTH32F_STENCIL8, 0, 1, 0, 1);
	glCheck();
	glBindTexture(GL_TEXTURE_2D, depthTexId);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glCheck();

	uStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	verify(uStatus == GL_FRAMEBUFFER_COMPLETE);

	/* The copy of the depth a frame of more than one pass draws its later
	 * passes against, made with the rest (not by the first such frame) */
	if (depth_fbo == 0)
		glGenFramebuffers(1, &depth_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo);
	depthSaveTexId = glc_gen_texture();
	glBindTexture(GL_TEXTURE_2D, depthSaveTexId);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, width, height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthSaveTexId, 0); glCheck();
	uStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	verify(uStatus == GL_FRAMEBUFFER_COMPLETE);
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
}

void gl4DrawStrips(GLuint output_fbo, int width, int height)
{
	u32 bands, bandCount, band;
	int bandHeight, render_pass_count;

	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
	if (texSamplers[0] == 0)
	{
		glGenSamplers(2, texSamplers);
		samplerSet[0] = samplerSet[1] = 0;
	}

	if (!pvrrc.isRTT && pvrrc.clearFramebuffer)
	{
		/* a framebuffer the game has not drawn to lately: nothing of the last picture belongs in it */
		glc_disable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glc_clear_color(VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (gl4ShaderUniforms.base_clipping.enabled)
			glc_enable(GL_SCISSOR_TEST);
	}
	else if (!pvrrc.isRTT && !working_holds_screen)
	{
		/* A render that covers only part of the screen leaves the rest as
		 * it was - which is as it is in this working buffer, and that is
		 * the screen's last picture only if nothing else has been drawn
		 * through it since. A render to a texture has, or the buffer is
		 * new: the screen's picture is put back first. (Every frame would
		 * do, as upstream has it; it is only needed then.) */
		glc_disable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, geom_fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, output_fbo);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
		if (gl4ShaderUniforms.base_clipping.enabled)
			glc_enable(GL_SCISSOR_TEST);
	}
	working_holds_screen = !pvrrc.isRTT;
	glc_depth_mask(GL_TRUE);
	glClearDepth(0.0);
	glc_stencil_mask(0xFF);
	glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();

	/* Draw the strips ! */

	/* We use sampler 0 */
	glActiveTexture(GL_TEXTURE0);
	glProvokingVertex(GL_LAST_VERTEX_CONVENTION);

	/* In bands, when the frames before had more translucent fragments than
	 * the lists have room for (abuffer.cpp): all of the frame's render
	 * passes once a band, every draw and clear cut to the band by the
	 * scissor, each band's lists starting empty and put on the picture
	 * before the next band's. Nothing is dropped and nothing is allocated;
	 * the opaque polygons are drawn once a band. */
	{
		const u32 chosen = gl4OitChooseBands(pvrrc.isRTT);
		const u32 rows = (u32)(height > 1 ? height : 1);
		bands = chosen < rows ? chosen : rows;
		if (bands < 1)
			bands = 1;
	}
	bandHeight = (height + (int)bands - 1) / (int)bands;
	bandCount = (u32)((height + bandHeight - 1) / bandHeight);
	render_pass_count = TA_LIST_USED(pvrrc.render_passes, RenderPass);

	for (band = 0; band < bandCount; band++)
	{
	RenderPass previous_pass;
	int render_pass;
	gl4band.y = (int)band * bandHeight;
	gl4band.height = bandCount > 1 ? (bandHeight < height - gl4band.y ? bandHeight : height - gl4band.y) : 0;
	gl4OitBandBegin();
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
	/* (the modifier volumes of the band before left the stencil mask at their bits) */
	glc_stencil_mask(0xFF);
	memset(&previous_pass, 0, sizeof(previous_pass));

	for (render_pass = 0; render_pass < render_pass_count; render_pass++)
	{
		const RenderPass current_pass = TA_LIST_HEAD(pvrrc.render_passes, RenderPass)[render_pass];
		/* Check if we can skip this pass, in part or completely, in case nothing is drawn (Cosmic Smash) */
		bool skip_op_pt = true;
		bool skip_tr = true;
		u32 j;
		for (j = previous_pass.op_count; skip_op_pt && j < current_pass.op_count; j++)
		{
			if (TA_LIST_HEAD(pvrrc.global_param_op, PolyParam)[j].count > 2)
				skip_op_pt = false;
		}
		for (j = previous_pass.pt_count; skip_op_pt && j < current_pass.pt_count; j++)
		{
			if (TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam)[j].count > 2)
				skip_op_pt = false;
		}
		for (j = previous_pass.tr_count; skip_tr && j < current_pass.tr_count; j++)
		{
			if (TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam)[j].count > 2)
				skip_tr = false;
		}
		if (skip_op_pt && skip_tr)
		{
			previous_pass = current_pass;
			continue;
		}
		DEBUG_LOG(RENDERER, "Render pass %d/%d OP %d PT %d TR %d autosort %d\n", render_pass + 1, render_pass_count,
				current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.autosort);

		glBindVertexArray(gl4.vbo.main_vao);

		if (!skip_op_pt)
		{
			/* PASS 1: Geometry pass to update depth and stencil */
			if (render_pass > 0)
			{
				/* Make a copy of the depth buffer that will be reused in pass 2 */
				/* (into the copy made with the textures: gl4CreateTextures()) */
				glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, geom_fbo);
				glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
				glCheck();

				glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
			}
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glc_enable(GL_DEPTH_TEST);
			glc_depth_mask(GL_TRUE);
			glc_enable(GL_STENCIL_TEST);
			glc_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);

			DrawList(ListType_Opaque, false, PASS_DEPTH, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), previous_pass.op_count, current_pass.op_count - previous_pass.op_count);
			DrawList(ListType_Punch_Through, false, PASS_DEPTH, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

			/* Modifier volumes */
			DrawModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count);

			/* PASS 2: Render OP and PT to fbo */
			if (render_pass == 0)
			{
				glc_depth_mask(GL_TRUE);
				glClearDepth(0.0);
				glClear(GL_DEPTH_BUFFER_BIT);
			}
			else
			{
				/* Restore the depth buffer from the last render pass */
				/* FIXME This is pretty slow apparently (CS) */
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, geom_fbo);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
				glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
				glCheck();
				glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
			}

			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glc_disable(GL_STENCIL_TEST);

			/* Bind stencil buffer for the fragment shader (shadowing) */
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, stencilTexId);
			glActiveTexture(GL_TEXTURE0);
			glCheck();

			/* Opaque */
			DrawList(ListType_Opaque, false, PASS_COLOR, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), previous_pass.op_count, current_pass.op_count - previous_pass.op_count);

			/* Alpha tested */
			DrawList(ListType_Punch_Through, false, PASS_COLOR, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

			/* Unbind stencil */
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, 0);
			glActiveTexture(GL_TEXTURE0);
		}

		if (!skip_tr)
		{
			/* PASS 3: Render TR to a-buffers */
			if (current_pass.autosort)
			{
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				glc_disable(GL_DEPTH_TEST);

				/* Although the depth test is disabled and thus writes to the depth buffer are also disabled, */
				/* AMD cards have serious issues when the depth/stencil texture is still bound to the framebuffer */
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
				glActiveTexture(GL_TEXTURE2);
				glBindTexture(GL_TEXTURE_2D, depthTexId);
				glActiveTexture(GL_TEXTURE0);
				DrawList(ListType_Translucent, true, PASS_OIT, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);

				/* Translucent modifier volumes */
				SetBaseClipping();
				if (current_pass.mv_op_tr_shared)
					DrawTranslucentModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, true);
				else
					DrawTranslucentModVols(previous_pass.mvo_tr_count, current_pass.mvo_tr_count - previous_pass.mvo_tr_count, false);

				/* Rebind the depth/stencil texture to the framebuffer */
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencilTexId, 0);

				if (render_pass < render_pass_count - 1)
				{
					/* PASS 3b: Geometry pass with TR to update the depth for the next TA render pass */
					/* Unbind depth texture */
					glActiveTexture(GL_TEXTURE2);
					glBindTexture(GL_TEXTURE_2D, 0);
					glActiveTexture(GL_TEXTURE0);

					glc_enable(GL_DEPTH_TEST);
					DrawList(ListType_Translucent, true, PASS_DEPTH, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				}
			}
			else
			{
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glc_enable(GL_DEPTH_TEST);
				DrawList(ListType_Translucent, false, PASS_COLOR, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				glc_disable(GL_BLEND);
			}
			glCheck();

			if (render_pass < render_pass_count - 1)
			{
				/* PASS 3c: Render a-buffer to temporary texture */
				const GLuint texId = CreateColorFBOTexture(max_image_width, max_image_height);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

				glActiveTexture(GL_TEXTURE0);
				glBindSampler(0, 0);
				glBindTexture(GL_TEXTURE_2D, opaqueTexId);

				renderABuffer();

				glc_delete_textures(1, &opaqueTexId);
				opaqueTexId = texId;

				glCheck();
			}
		}

		if (!skip_op_pt && render_pass < render_pass_count - 1)
		{
			/* Clear the stencil from this pass */
			glc_stencil_mask(0xFF);
			glClear(GL_STENCIL_BUFFER_BIT);
		}

		previous_pass = current_pass;
	}

	/* PASS 4: Render a-buffers to screen */
	glBindFramebuffer(GL_FRAMEBUFFER, output_fbo); glCheck();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, opaqueTexId);
	renderABuffer();
	gl4OitBandEnd(band);
	}
	gl4band.height = 0;
	gl4OitFrameEnd(bandCount, pvrrc.isRTT);
}

/* A quad's four vertices, white, at depth 1, drawn as a strip of 0, 1, 2, 1, 3 */
static void QuadVertices(Vertex *v, float x, float y, float w, float h)
{
	int i;
	memset(v, 0, 4 * sizeof(*v));
	for (i = 0; i < 4; i++)
	{
		v[i].x = (i & 2) ? x + w : x;
		v[i].y = (i & 1) ? y : y + h;
		v[i].z = 1;
		v[i].col[0] = v[i].col[1] = v[i].col[2] = v[i].col[3] = 255;
		v[i].u = (i & 2) ? 1.f : 0.f;
		v[i].v = (i & 1) ? 0.f : 1.f;
	}
}

static const GLushort quad_indices[5] = { 0, 1, 2, 1, 3 };

static void gl4_draw_quad_texture(GLuint texture, float w, float h)
{
	Vertex vertices[4];

	glc_disable(GL_SCISSOR_TEST);
	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);
	glc_disable(GL_CULL_FACE);
	glc_disable(GL_BLEND);

	gl4ShaderUniforms.trilinear_alpha = 1.0;

	CurrentShader = gl4GetProgram(false,
				false,
				true,
				false,
				true,
				0,
				false,
				2,
				false,
				false,
				false,
				false,
				false,
				PASS_COLOR);
	glc_use_program(CurrentShader->program);
	gl4_uniforms_set(CurrentShader);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	gl4BindFrameBuffers(false, true);

	QuadVertices(vertices, 0, 0, w, h);
	glBindVertexArray(gl4.vbo.main_vao);
	glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STREAM_DRAW);

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);
}

void gl4DrawFramebuffer(float w, float h)
{
	/* (the texture is kept for the next: see RenderFramebuffer()) */
	gl4_draw_quad_texture(fbTextureId, w, h);
}

void gl4DrawVmuTexture(u8 vmu_screen_number)
{
	bool vmu_lcd_changed;
	float h;
	float w;
	float y;
	float x;
	glActiveTexture(GL_TEXTURE0);

	x = 0 ;
	y = 0 ;
	w = VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_number].vmu_screen_size_mult ;
	h = VMU_SCREEN_HEIGHT*vmu_screen_params[vmu_screen_number].vmu_screen_size_mult ;

	/* A new picture from the game, new colours from the options, or no texture yet */
	vmu_lcd_changed = vmu_lcd_refresh(vmu_screen_number);
	if (vmu_lcd_changed || vmu_screen_params[vmu_screen_number].vmu_screen_needs_update || vmuTextureId[vmu_screen_number] == 0)
		UpdateVmuTexture(vmu_screen_number) ;

	switch ( vmu_screen_params[vmu_screen_number].vmu_screen_position )
	{
		case UPPER_LEFT :
		{
			x = 0 ;
			y = 0 ;
			break ;
		}
		case UPPER_RIGHT :
		{
			x = 640-w ;
			y = 0 ;
			break ;
		}
		case LOWER_LEFT :
		{
			x = 0 ;
			y = 480-h ;
			break ;
		}
		case LOWER_RIGHT :
		{
			x = 640-w ;
			y = 480-h ;
			break ;
		}
	}

   glc_bind_texture(GL_TEXTURE_2D, vmuTextureId[vmu_screen_number]);

	glc_disable(GL_SCISSOR_TEST);
	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);
	glc_disable(GL_CULL_FACE);
   glc_enable(GL_BLEND);
   glc_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   gl4BindFrameBuffers(false, true);
   glBindVertexArray(gl4.vbo.main_vao);

	gl4ShaderUniforms.trilinear_alpha = 1.0;

	CurrentShader = gl4GetProgram(false,
				0,
				true,
				true,
				false,
				0,
				false,
				2,
				false,
				false,
				false,
				false,
				false,
				PASS_COLOR);
	glc_use_program(CurrentShader->program);
	gl4_uniforms_set(CurrentShader);

	{
		Vertex vertices[4];
		QuadVertices(vertices, x, y, w, h);
		glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STREAM_DRAW);
	}

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);
}

static void gl4UpdateLightGunTexture(int port)
{
	/* (the renderer's thread's) */
	static u8 temp_tex_buffer[LIGHTGUN_CROSSHAIR_SIZE*LIGHTGUN_CROSSHAIR_SIZE*4];
	s32 x,y ;
	u8 *dst = temp_tex_buffer;
	const u8 *src = NULL ;
	const u8 *colour;

	if (lightgunTextureId[port] == 0)
	{
		lightgunTextureId[port] = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, lightgunTextureId[port]);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
		glc_bind_texture(GL_TEXTURE_2D, lightgunTextureId[port]);

	colour = &( lightgun_palette[ lightgun_params[port].colour * 3 ] );

	for ( y = LIGHTGUN_CROSSHAIR_SIZE-1 ; y >= 0 ; y--)
	{
	   src = lightgun_img_crosshair + (y*LIGHTGUN_CROSSHAIR_SIZE) ;

	   for ( x = 0 ; x < LIGHTGUN_CROSSHAIR_SIZE ; x++)
	   {
		   if ( src[x] )
		   {
			  *dst++ = colour[0] ;
			  *dst++ = colour[1] ;
			  *dst++ = colour[2] ;
			  *dst++ = 0xFF ;
		   }
		   else
		   {
			  *dst++ = 0 ;
			  *dst++ = 0 ;
			  *dst++ = 0 ;
			  *dst++ = 0 ;
		   }
	   }
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, temp_tex_buffer);

	lightgun_params[port].dirty = false;
}

void gl4DrawGunCrosshair(u8 port)
{
	float h;
	float w;
	float y;
	float x;
	if ( lightgun_params[port].offscreen || (lightgun_params[port].colour==0) )
		return;

	glActiveTexture(GL_TEXTURE0);

	x = 0;
	y = 0;
	w = LIGHTGUN_CROSSHAIR_SIZE;
	h = LIGHTGUN_CROSSHAIR_SIZE;

	x = lightgun_params[port].x - ( LIGHTGUN_CROSSHAIR_SIZE / 2 );
	y = lightgun_params[port].y - ( LIGHTGUN_CROSSHAIR_SIZE / 2 );

	if (lightgun_params[port].dirty || lightgunTextureId[port] == 0)
		gl4UpdateLightGunTexture(port);

	glc_bind_texture(GL_TEXTURE_2D, lightgunTextureId[port]);

	glc_disable(GL_SCISSOR_TEST);
	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);
	glc_disable(GL_CULL_FACE);
   glc_enable(GL_BLEND);
	glc_blend_func(GL_SRC_ALPHA, GL_ONE);

	gl4BindFrameBuffers(false, true);
	glBindVertexArray(gl4.vbo.main_vao);

	gl4ShaderUniforms.trilinear_alpha = 1.0;
	CurrentShader = gl4GetProgram(false,
				0,
				true,
				true,
				false,
				0,
				false,
				2,
				false,
				false,
				false,
				false,
				false,
				PASS_COLOR);
	glc_use_program(CurrentShader->program);
	gl4_uniforms_set(CurrentShader);

	{
		Vertex vertices[4];
		QuadVertices(vertices, x, y, w, h);
		glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STREAM_DRAW);
	}

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);

	glc_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
