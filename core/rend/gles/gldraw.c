#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/* Drawing and related state management: takes vertices and textures and
 * renders to the target set up */
#include <string.h>
#include "gles.h"
#include "rend/grow_buf.h"
#include "ta_decoder_gl.h"
#include "rend/shadows.h"
#include "rend/modvol_clip.h"

/* (a polygon's control words, in C their u32s) */
#define PPW(gp, w, f) TAW_GET((gp)->w, f)

static const u32 CullModes[] =
{
	GL_NONE,	/* 0 No culling */
	GL_NONE,	/* 1 Cull if small: cull if (|det| < fpu_cull_val) */
	GL_FRONT,	/* 2 Cull if negative: cull if (|det| < 0) or (|det| < fpu_cull_val) */
	GL_BACK		/* 3 Cull if positive: cull if (|det| > 0) or (|det| < fpu_cull_val) */
};
const u32 Zfunction[8] =
{
	GL_NEVER,	/* 0 Never */
	GL_LESS,	/* 1 Less */
	GL_EQUAL,	/* 2 Equal */
	GL_LEQUAL,	/* 3 Less or equal */
	GL_GREATER,	/* 4 Greater */
	GL_NOTEQUAL,	/* 5 Not equal */
	GL_GEQUAL,	/* 6 Greater or equal */
	GL_ALWAYS	/* 7 Always */
};

/*
0   Zero                  (0, 0, 0, 0)
1   One                   (1, 1, 1, 1)
2   Other Color           (OR, OG, OB, OA)
3   Inverse Other Color   (1-OR, 1-OG, 1-OB, 1-OA)
4   SRC Alpha             (SA, SA, SA, SA)
5   Inverse SRC Alpha     (1-SA, 1-SA, 1-SA, 1-SA)
6   DST Alpha             (DA, DA, DA, DA)
7   Inverse DST Alpha     (1-DA, 1-DA, 1-DA, 1-DA)
*/
const u32 DstBlendGL[8] =
{
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

const u32 SrcBlendGL[8] =
{
	GL_ZERO,
	GL_ONE,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

static PipelineShader *CurrentShader;
GLuint vmuTextureId[4] = { 0, 0, 0, 0 };
GLuint lightgunTextureId[4] = { 0, 0, 0, 0 };

/* bytes an index */
#define GL_INDEX_SIZE() (gl.index_type == GL_UNSIGNED_INT ? sizeof(u32) : sizeof(u16))

void shader_uniforms_set(PipelineShader *s)
{
	if (s->shade_scale_factor != -1)
		glUniform1f(s->shade_scale_factor, ShaderUniforms.shade_scale_factor);
	if (s->cp_AlphaTestValue != -1)
		glUniform1f(s->cp_AlphaTestValue, ShaderUniforms.PT_ALPHA);
	if (s->depth_scale != -1)
		glUniform4fv(s->depth_scale, 1, ShaderUniforms.depth_coefs);
	if (s->sp_FOG_DENSITY != -1)
		glUniform1f(s->sp_FOG_DENSITY, ShaderUniforms.fog_den_float);
	if (s->sp_FOG_COL_RAM != -1)
		glUniform3fv(s->sp_FOG_COL_RAM, 1, ShaderUniforms.ps_FOG_COL_RAM);
	if (s->sp_FOG_COL_VERT != -1)
		glUniform3fv(s->sp_FOG_COL_VERT, 1, ShaderUniforms.ps_FOG_COL_VERT);
	if (s->fog_clamp_min != -1)
		glUniform4fv(s->fog_clamp_min, 1, ShaderUniforms.fog_clamp_min);
	if (s->fog_clamp_max != -1)
		glUniform4fv(s->fog_clamp_max, 1, ShaderUniforms.fog_clamp_max);
	if (s->normal_matrix != -1)
		glUniformMatrix4fv(s->normal_matrix, 1, GL_FALSE, ShaderUniforms.normal_mat);
	if (s->palette_index != -1)
	{
		glUniform1i(s->palette_index, ShaderUniforms.palette_index);
		s->shadow_palette = ShaderUniforms.palette_index;
		s->shadow_set |= PIPELINE_SHADOW_PALETTE;
	}
}

void SetCull(u32 CullMode)
{
	if (CullModes[CullMode] == GL_NONE)
		glc_disable(GL_CULL_FACE);
	else
	{
		glc_enable(GL_CULL_FACE);
		glc_cull_face(CullModes[CullMode]);
	}
}

static void SetTextureRepeatMode(GLuint dir, u32 clamp, u32 mirror)
{
	if (clamp)
		glc_tex_parameteri(GL_TEXTURE_2D, dir, GL_CLAMP_TO_EDGE);
	else
		glc_tex_parameteri(GL_TEXTURE_2D, dir, mirror ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

static void SetBaseClipping(void)
{
	if (ShaderUniforms.base_clipping.enabled)
	{
		glc_enable(GL_SCISSOR_TEST);
		glc_scissor(ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y,
				ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
	}
	else
		glc_disable(GL_SCISSOR_TEST);
}

/* A polygon's state, of list @Type, @SortingEnabled (each call gives them
 * as constants: what does not apply to its list goes) */
static INLINE void SetGPState(u32 Type, bool SortingEnabled, const PolyParam *gp, u32 cflip, bool shadowed)
{
	const bool color_clamp = PPW(gp, tsp, TSP_ColorClamp) && (pvrrc.fog_clamp_min != 0 || pvrrc.fog_clamp_max != 0xffffffff);
	const int fog_ctrl = PPW(gp, tsp, TSP_FogCtrl);
	const bool palette = tex_gpu_paletted(gp->tsp, gp->tcw, 0);
	int clip_rect[4] = { 0, 0, 0, 0 };
	int clipmode;
	PipelineShader *owner;
	u32 stencil;
	bool mipmapped;

	if (PPW(gp, pcw, PCW_Texture) && PPW(gp, tsp, TSP_FilterMode) > 1 && Type != ListType_Punch_Through
			&& PPW(gp, tcw, TCW_MipMapped) == 1)
	{
		ShaderUniforms.trilinear_alpha = 0.25 * (PPW(gp, tsp, TSP_MipMapD) & 0x3);
		/* trilinear pass A */
		if (PPW(gp, tsp, TSP_FilterMode) == 2)
			ShaderUniforms.trilinear_alpha = 1.0 - ShaderUniforms.trilinear_alpha;
	}
	else
		ShaderUniforms.trilinear_alpha = 1.f;

	clipmode = tile_clip(gp->tileclip, &ViewportMatrix, clip_rect);

	CurrentShader = GetProgram(Type == ListType_Punch_Through,
			clipmode == TILECLIP_INSIDE,
			PPW(gp, pcw, PCW_Texture),
			PPW(gp, tsp, TSP_UseAlpha),
			PPW(gp, tsp, TSP_IgnoreTexA),
			PPW(gp, tsp, TSP_ShadInstr),
			PPW(gp, pcw, PCW_Offset),
			fog_ctrl,
			PPW(gp, pcw, PCW_Gouraud),
			PPW(gp, tcw, TCW_PixelFmt) == TA_PIXEL_BUMPMAP,
			color_clamp,
			ShaderUniforms.trilinear_alpha != 1.f,
			palette,
			shadowed);

	UsePipelineShader(CurrentShader);
	/* (the uniforms are the program's: those of the record whose program it is) */
	owner = CurrentShader->general != NULL ? CurrentShader->general : CurrentShader;
	if (CurrentShader->trilinear_alpha != -1
			&& (!(owner->shadow_set & PIPELINE_SHADOW_TRILINEAR)
				|| memcmp(&owner->shadow_trilinear, &ShaderUniforms.trilinear_alpha, sizeof(float)) != 0))
	{
		glUniform1f(CurrentShader->trilinear_alpha, ShaderUniforms.trilinear_alpha);
		owner->shadow_trilinear = ShaderUniforms.trilinear_alpha;
		owner->shadow_set |= PIPELINE_SHADOW_TRILINEAR;
	}
	if (palette)
	{
		if (PPW(gp, tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
			ShaderUniforms.palette_index = PPW(gp, tcw, TCW_PalSelect) << 4;
		else
			ShaderUniforms.palette_index = (PPW(gp, tcw, TCW_PalSelect) >> 4) << 8;
		if (!(owner->shadow_set & PIPELINE_SHADOW_PALETTE) || owner->shadow_palette != ShaderUniforms.palette_index)
		{
			glUniform1i(CurrentShader->palette_index, ShaderUniforms.palette_index);
			owner->shadow_palette = ShaderUniforms.palette_index;
			owner->shadow_set |= PIPELINE_SHADOW_PALETTE;
		}
	}

	if (clipmode == TILECLIP_INSIDE)
	{
		float clip[4];
		clip[0] = (float)clip_rect[0];
		clip[1] = (float)clip_rect[1];
		clip[2] = (float)(clip_rect[0] + clip_rect[2]);
		clip[3] = (float)(clip_rect[1] + clip_rect[3]);
		if (!(owner->shadow_set & PIPELINE_SHADOW_CLIP) || memcmp(owner->shadow_clip, clip, sizeof(clip)) != 0)
		{
			glUniform4f(CurrentShader->pp_ClipTest, clip[0], clip[1], clip[2], clip[3]);
			memcpy(owner->shadow_clip, clip, sizeof(clip));
			owner->shadow_set |= PIPELINE_SHADOW_CLIP;
		}
	}
	if (clipmode == TILECLIP_OUTSIDE)
	{
		if (ShaderUniforms.base_clipping.enabled)
			intersect_clip_rect(clip_rect, ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y,
					ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
		glc_enable(GL_SCISSOR_TEST);
		glc_scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
	}
	else
		SetBaseClipping();

	/* this bit controls which pixels are affected by modifier volumes */
	stencil = PPW(gp, pcw, PCW_Shadow) != 0 ? 0x80 : 0x0;
	glc_stencil_func(GL_ALWAYS, stencil, stencil);

	glc_bind_texture(GL_TEXTURE_2D, gp->texid == (u64)-1 ? 0 : (GLuint)gp->texid);

	SetTextureRepeatMode(GL_TEXTURE_WRAP_S, PPW(gp, tsp, TSP_ClampU), PPW(gp, tsp, TSP_FlipU));
	SetTextureRepeatMode(GL_TEXTURE_WRAP_T, PPW(gp, tsp, TSP_ClampV), PPW(gp, tsp, TSP_FlipV));

	/* The texture's filter. Mipmapping does not depend on the filter: the
	 * chip picks a level by how small the texture is drawn, and filters
	 * within it or does not. (A texture of palette indices is never one with
	 * mipmaps.) As the texture cache decides it, IsMipmapped(). */
	mipmapped = !palette && PPW(gp, tcw, TCW_MipMapped) != 0 && !settings.rend.ForceTextureLOD0
			&& (PPW(gp, tcw, TCW_ScanOrder) == 0 || PPW(gp, tcw, TCW_PixelFmt) == TA_PIXEL_4BPP
				|| PPW(gp, tcw, TCW_PixelFmt) == TA_PIXEL_8BPP);
#ifdef GL_TEXTURE_LOD_BIAS
	if (!gl.is_gles && gl.gl_major >= 3 && mipmapped)
		glc_tex_parameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, D_Adjust_LoD_Bias[PPW(gp, tsp, TSP_MipMapD)]);
#endif
	if (PPW(gp, tsp, TSP_FilterMode) == 0 || palette)
	{
		/* No filtering - the level is still picked. This was the largest
		 * level always, where Vulkan and the console use the one that
		 * fits. And no anisotropy: a texture drawn filtered before keeps
		 * that setting, and with it would be filtered here all the same. */
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (gl.max_anisotropy > 1.f)
			glc_tex_parameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
	}
	else
	{
		/* bilinear filtering (the PowerVR does trilinear too, in two
		 * passes: not done here) */
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		if (gl.max_anisotropy > 1.f)
		{
			if (settings.rend.AnisotropicFiltering > 1)
			{
				const f32 aniso = (f32)settings.rend.AnisotropicFiltering;
				glc_tex_parameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
						aniso < gl.max_anisotropy ? aniso : gl.max_anisotropy);
				/* Set the recommended minification filter for best results.
				 * Not for a punch-through polygon: blending two levels
				 * blends their alpha, and what is cut out by alpha is then
				 * cut out wrong (upstream: the "Press Start" of Trizeal's
				 * title screen, Virtua Tennis's). */
				if (mipmapped && Type != ListType_Punch_Through)
					glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			}
			else
				glc_tex_parameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
		}
	}

	/* Punch-through polygons blend too, or some combinations at least. An
	 * opaque polygon is blended by its instructions too when it is in a
	 * list continuation; the ones of the first render pass have had theirs
	 * set to source x 1 and destination x 0 when the list was read. That
	 * pair is no blending at all, and is left as that so it costs nothing. */
	if (Type == ListType_Translucent || Type == ListType_Punch_Through
			|| PPW(gp, tsp, TSP_SrcInstr) != 1 || PPW(gp, tsp, TSP_DstInstr) != 0)
	{
		glc_enable(GL_BLEND);
		glc_blend_func(SrcBlendGL[PPW(gp, tsp, TSP_SrcInstr)], DstBlendGL[PPW(gp, tsp, TSP_DstInstr)]);
	}
	else
		glc_disable(GL_BLEND);

	/* The cull mode: @cflip for triangles drawn sorted, gcflip the whole
	 * picture's, for renders to textures (their Y is mirrored) */
	SetCull(PPW(gp, isp, ISP_TSP_CullMode) ^ cflip ^ gcflip);

	if (Type == ListType_Punch_Through || (Type == ListType_Translucent && SortingEnabled))
		glc_depth_func(GL_GEQUAL);
	else
		glc_depth_func(Zfunction[PPW(gp, isp, ISP_TSP_DepthMode)]);

	/* Sorted translucent polygons, per triangle or per strip, do not write
	 * depth: a strip drawn in sorted order would hide parts of the ones after it. */
	if (SortingEnabled)
		glc_depth_mask(GL_FALSE);
	/* (Z Write Disable seems to be ignored for punch-through: Worms World
	 * Party, Bust-a-Move 4 and Re-Volt) */
	else if (Type == ListType_Punch_Through)
		glc_depth_mask(GL_TRUE);
	else
		glc_depth_mask(!PPW(gp, isp, ISP_TSP_ZWriteDis));
}

static void DrawList(u32 Type, bool SortingEnabled, const PolyParam *head, int first, int count)
{
	const PolyParam *params = head + first;

	if (count == 0)
		return;

	/* set some 'global' modes for all primitives */
	glc_enable(GL_STENCIL_TEST);
	glc_stencil_func(GL_ALWAYS, 0, 0);
	glc_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
	/* Opaque and punch-through polygons say in the stencil buffer whether
	 * what is showing at a pixel takes shadows; translucent ones leave it
	 * alone. The mask has to be set here: the modifier volumes of the pass
	 * before leave it letting through their own bits only, and a polygon
	 * of this pass drawn over one that takes shadows was then taken for
	 * it. */
	glc_stencil_mask(Type == ListType_Translucent ? 0 : 0xFF);

	for (; count > 0; count--, params++)
	{
		/* (this actually happens for some games) */
		if (params->count <= 2)
			continue;
		SetGPState(Type, SortingEnabled, params, 0, false);
		/* (of a frame decoded on the GPU, a draw group: see TA_GPU_DRAW) */
		if (pvrrc.gpu)
			gltad_draw_group(Type, params->first, params->count, false);
		else
			glDrawElements(GL_TRIANGLE_STRIP, params->count, gl.index_type,
					(GLvoid *)(GL_INDEX_SIZE() * params->first));
	}
}

/* The sorted triangles' draws and the depth prepass's (rend/sorter.h),
 * its indices after the sorted ones; kept from one frame to the next */
static SortList sortedDraws, prepassDraws;
/* (kept from one frame to the next: grown, never made again) */
static grow_buf_t sortedIndices;

void gl_sort_reserve(void)
{
	SortListReserve(&sortedDraws);
	SortListReserve(&prepassDraws);
	SortIndicesReserve(&sortedIndices);
}

/* (where the shaders write the depth: OpenGL ES 2 has them leave it, and a discard there would cost every polygon) */
static bool PrepassSupported(void)
{
	return strcmp(gl.gl_version, "GLES2") != 0;
}

/* The sorted triangles of the render pass's translucent list, when they
 * are drawn by triangle, and its depth prepass's: written after the
 * frame's indices where the frame was parsed into the buffers the card
 * reads, and bound, or uploaded to a buffer of their own. Where they start
 * in the buffer bound, in bytes. */
static uintptr_t SortTriangles(int first, int count, bool sortTriangles, bool lastPass)
{
	const bool prepass = PrepassSupported();
	size_t room, at = 0;
	u32 *here, *out;
	u32 n = 0;

	sortedDraws.count = 0;
	prepassDraws.count = 0;
	if (!sortTriangles && !prepass)
		return 0;
	/* room for both, made u16 in place if the indices are */
	room = (size_t)(6 * SortedTriangles(first, count) + 1) * sizeof(u32);
	here = gl_frame_sorted(room, &at);
	out = here != NULL ? here : (u32 *)grow_buf_reserve(&sortedIndices, room);
	if (out == NULL)
		return 0;
	if (sortTriangles)
		n = GenSortedTo(first, count, &sortedDraws, out, false);
	if (prepass)
		n += GenPrepassTo(first, count, lastPass, &prepassDraws, out + n, n, false);
	if (n == 0)
		return 0;
	if (here != NULL)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_frame_index_buffer(gl.vbo.idxs));
		return at;
	}

	/* the sorted indices uploaded to a buffer of their own */
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs2);
	if (gl.index_type == GL_UNSIGNED_SHORT)
	{
		/* (narrowed where they are: each u16 is written at or before the u32 it is read from) */
		u16 *to = (u16 *)out;
		u32 i;
		for (i = 0; i < n; i++)
			to[i] = (u16)out[i];
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * sizeof(u16), to, GL_STREAM_DRAW);
	}
	else
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * sizeof(u32), out, GL_STREAM_DRAW);
	return 0;
}

/* The depth prepass of a sorted list: rend/sorter.h (with the sorted
 * indices bound, @base bytes in: @d's first counts from there) */
static void DrawPrepass(const SortTrigDrawParam *draws, u32 count, uintptr_t base)
{
	u32 i;
	if (count == 0)
		return;
	glc_enable(GL_STENCIL_TEST);
	glc_stencil_func(GL_ALWAYS, 0, 0);
	glc_stencil_op(GL_KEEP, GL_KEEP, GL_KEEP);
	glc_stencil_mask(0);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	for (i = 0; i < count; i++)
	{
		const SortTrigDrawParam *d = &draws[i];
		SetGPState(ListType_Translucent, true, d->ppid, 0, false);
		glc_depth_mask(GL_TRUE);
		glc_disable(GL_BLEND);
		glUniform1f(CurrentShader->prepass_alpha, PrepassAlwaysHides(d->ppid) ? -1.f : PREPASS_ALPHA_MIN);
		glDrawElements(GL_TRIANGLES, d->count, gl.index_type, (GLvoid *)(base + GL_INDEX_SIZE() * d->first));
		/* (the colour pass draws with the same program) */
		glUniform1f(CurrentShader->prepass_alpha, 0.f);
	}
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glc_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
}

/* The sorted triangles of a list, @count draws of them (with the sorted
 * indices bound, @base bytes in: as DrawPrepass()) */
static void DrawSorted(const SortTrigDrawParam *pidx_sort, u32 count, bool multipass, uintptr_t base)
{
	u32 p;
	if (count == 0)
		return;

	/* set some 'global' modes for all primitives */
	glc_enable(GL_STENCIL_TEST);
	glc_stencil_func(GL_ALWAYS, 0, 0);
	glc_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
	/* translucent: the stencil buffer is left alone (see DrawList) */
	glc_stencil_mask(0);

	for (p = 0; p < count; p++)
	{
		/* (this actually happens for some games) */
		if (pidx_sort[p].count <= 2)
			continue;
		SetGPState(ListType_Translucent, true, pidx_sort[p].ppid, 0, false);
		glDrawElements(GL_TRIANGLES, pidx_sort[p].count, gl.index_type,
				(GLvoid *)(base + GL_INDEX_SIZE() * pidx_sort[p].first));
	}

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		/* Write to the depth buffer now: the next render pass might need it
		 * (Cosmic Smash) */
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glc_disable(GL_BLEND);

		glc_stencil_mask(0);

		/* (the modifier volumes' program: fast, and no textures are needed) */
		glc_use_program(gl.modvol_shader.program);
		glUniform1f(gl.modvol_shader.sp_ShaderColor, 1.f);

		glc_depth_func(GL_GEQUAL);
		glc_depth_mask(GL_TRUE);

		for (p = 0; p < count; p++)
		{
			const PolyParam *params = pidx_sort[p].ppid;
			if (pidx_sort[p].count > 2 && !PPW(params, isp, ISP_TSP_ZWriteDis))
			{
				/* (FIXME: no clipping in the modifier volumes' program) */
				SetCull(PPW(params, isp, ISP_TSP_CullMode) ^ gcflip);
				glDrawElements(GL_TRIANGLES, pidx_sort[p].count, gl.index_type,
						(GLvoid *)(base + GL_INDEX_SIZE() * pidx_sort[p].first));
			}
		}
		glc_stencil_mask(0xFF);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
}

/* All pixels are in area 0 by default. If inside an 'in' volume, they are
 * in area 1; if inside an 'out' volume, they are in area 0.

	Stencil bits:
		bit 7: mv affected (must be preserved)
		bit 1: current volume state
		bit 0: summary result (starts off as 0)

	Lower 2 bits:

	IN volume (logical OR):
	00 -> 00
	01 -> 01
	10 -> 01
	11 -> 01

	Out volume (logical AND):
	00 -> 00
	01 -> 00
	10 -> 00
	11 -> 01
*/
void gl_set_mvs_mode(int mv_mode, u32 cull_mode)
{
	if (mv_mode == Xor)
	{
		glc_enable(GL_DEPTH_TEST);
		/* write only bit 1 */
		glc_stencil_mask(2);
		/* no stencil testing */
		glc_stencil_func(GL_ALWAYS, 0, 2);
		/* count the number of pixels in front of the Z buffer (xor zpass) */
		glc_stencil_op(GL_KEEP, GL_KEEP, GL_INVERT);
		SetCull(cull_mode);
	}
	else if (mv_mode == Or)
	{
		glc_enable(GL_DEPTH_TEST);
		/* write only bit 1 */
		glc_stencil_mask(2);
		/* no stencil testing */
		glc_stencil_func(GL_ALWAYS, 2, 2);
		/* or'ing of all triangles */
		glc_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
		SetCull(cull_mode);
	}
	else
	{
		/* Inclusion or Exclusion volume: no depth test, bits 1:0 written */
		glc_disable(GL_DEPTH_TEST);
		glc_stencil_mask(3);

		if (mv_mode == Inclusion)
		{
			/* res : old : final
			 * 0   : 0   : 00
			 * 0   : 1   : 01
			 * 1   : 0   : 01
			 * 1   : 1   : 01
			 * if (1 <= st) st = 1; else st = 0; */
			glc_stencil_func(GL_LEQUAL, 1, 3);
			glc_stencil_op(GL_ZERO, GL_ZERO, GL_REPLACE);
		}
		else
		{
			/* Exclusion volume (Zombie Revenge, and a racing game). The
			 * initial value for exclusion volumes is 1, so the result is
			 * inverted before and'ing.
			 * res : old : final
			 * 0   : 0   : 00
			 * 0   : 1   : 01
			 * 1   : 0   : 00
			 * 1   : 1   : 00
			 * if (1 == st) st = 1; else st = 0; */
			glc_stencil_func(GL_EQUAL, 1, 3);
			glc_stencil_op(GL_ZERO, GL_ZERO, GL_KEEP);
		}
	}
}

/* The frame's vertices and indices: where it was parsed or copied to, or
 * (@gpu) those of GPU-side TA processing's decode; (@own) the renderer's
 * own buffers, for a quad of its own to be uploaded to and drawn */
static void SetupMainVBO(bool gpu, bool own)
{
	if (gpu)
		gltad_bind_geometry();
	else if (own)
	{
		glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.geometry);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs);
	}
	else
	{
		glBindBuffer(GL_ARRAY_BUFFER, gl_frame_vertex_buffer(gl.vbo.geometry));
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_frame_index_buffer(gl.vbo.idxs));
	}

	/* the vertex buffers' attributes */
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, x));

	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void *)offsetof(Vertex, col));

	glEnableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void *)offsetof(Vertex, vtx_spc));

	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, u));
}

static void SetupModvolVBO(bool gpu)
{
	if (gpu)
		gltad_bind_modvols();
	else
		glBindBuffer(GL_ARRAY_BUFFER, gl_frame_modvol_buffer(gl.vbo.modvols));

	/* the vertex buffers' attributes */
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void *)0);

	glDisableVertexAttribArray(VERTEX_UV_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
}

/* Shadows: see rend/shadows.h. */

/* The scissor for a polygon drawn again for its shadow: where it may be
 * drawn at all, and within the volumes' rectangle. False if that is nowhere. */
static bool SetShadowScissor(const PolyParam *gp, const int *volumes)
{
	int x0 = volumes[0], y0 = volumes[1], x1 = x0 + volumes[2], y1 = y0 + volumes[3];
	int clip[4] = { 0, 0, 0, 0 };
	bool clipped = true;

	if (tile_clip(gp->tileclip, &ViewportMatrix, clip) != TILECLIP_OUTSIDE)
	{
		clipped = ShaderUniforms.base_clipping.enabled;
		clip[0] = ShaderUniforms.base_clipping.x;
		clip[1] = ShaderUniforms.base_clipping.y;
		clip[2] = ShaderUniforms.base_clipping.width;
		clip[3] = ShaderUniforms.base_clipping.height;
	}
	else if (ShaderUniforms.base_clipping.enabled)
		intersect_clip_rect(clip, ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y,
				ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
	if (clipped)
	{
		if (x0 < clip[0])
			x0 = clip[0];
		if (y0 < clip[1])
			y0 = clip[1];
		if (x1 > clip[0] + clip[2])
			x1 = clip[0] + clip[2];
		if (y1 > clip[1] + clip[3])
			y1 = clip[1] + clip[3];
	}
	if (x1 <= x0 || y1 <= y0)
		return false;
	glc_enable(GL_SCISSOR_TEST);
	glc_scissor(x0, y0, x1 - x0, y1 - y0);
	return true;
}

static void DrawShadowed(u32 Type, const PolyParam *head, int first, int count, const ScreenBounds *area, const int *scissor)
{
	const PolyParam *gp = head + first;

	for (; count > 0; count--, gp++)
	{
		/* (of a frame decoded on the GPU, the GPU has said which groups
		 * reach into the volumes: the others' second commands draw nothing) */
		if (!CanDrawShadowed(Type, gp) || (!pvrrc.gpu && !poly_overlaps(gp, area)))
			continue;

		SetGPState(Type, false, gp, 0, true);
		if (!SetShadowScissor(gp, scissor))
			continue;
		glc_disable(GL_BLEND);
		glc_depth_func(GL_EQUAL);
		glc_depth_mask(GL_FALSE);
		/* In a volume (bit 0) and taking shadows (bit 7); bit 6 says a pixel
		 * has been drawn here already, and is set by this. */
		if (shadow_later_wins(Type, gp))
		{
			glc_stencil_func(GL_EQUAL, 0xC1, 0x81);
			glc_stencil_op(GL_KEEP, GL_KEEP, GL_REPLACE);
		}
		else
		{
			glc_stencil_func(GL_EQUAL, 0x81, 0xC1);
			glc_stencil_op(GL_KEEP, GL_KEEP, GL_INVERT);
		}
		if (pvrrc.gpu)
			gltad_draw_group(Type, gp->first, gp->count, true);
		else
			glDrawElements(GL_TRIANGLE_STRIP, gp->count, gl.index_type,
					(GLvoid *)(GL_INDEX_SIZE() * gp->first));
	}
}

static void DrawModVols(int first, int count, const RenderPass *previous_pass, const RenderPass *current_pass, int render_pass)
{
	/* A bit of explanation: in theory it works like this: generate a 1-bit
	 * stencil for each polygon volume, and then AND or OR it against the
	 * overall 1-bit tile stencil at the end of the volume. */
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *params = mvo + first;
	const f32 *ga;
	ScreenBounds area;
	int mod_base = -1;
	u32 clip = 0;	/* what the scissor is set for: so far nothing, the whole picture */
	int cmv;

	if (count == 0)
		return;

	SetupModvolVBO(pvrrc.gpu && TA_LIST_USED(pvrrc.mv_clip, ModTriangle) == 0);

	glc_disable(GL_BLEND);
	SetBaseClipping();

	glc_use_program(gl.modvol_shader.program);
	glUniform1f(gl.modvol_shader.sp_ShaderColor, 1 - FPU_SHAD_SCALE.scale_factor / 256.f);

	glc_enable(GL_DEPTH_TEST);
	glc_depth_mask(GL_FALSE);
	glc_depth_func(GL_GREATER);

	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode;
		mv_clip_range_get(&pvrrc, mvo, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = PPW(param, isp, ISP_Modvol_DepthMode);

		/* A volume is clipped like a polygon, to the rectangle it was sent
		 * under if it asked to be: its shadow fell outside it. (Kept in:
		 * the scissor. Kept out is not done, as it is not for upstream.) */
		if (param->tileclip != clip)
		{
			int clip_rect[4];
			clip = param->tileclip;
			if (tile_clip(clip, &ViewportMatrix, clip_rect) == TILECLIP_OUTSIDE)
			{
				glc_enable(GL_SCISSOR_TEST);
				glc_scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
			}
			else
				SetBaseClipping();
		}

		if (mod_base == -1)
			mod_base = mvFirst;

		/* or'ing (an open volume or a quad), else xor'ing (a closed volume) */
		if (!PPW(param, isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
			gl_set_mvs_mode(Or, PPW(param, isp, ISP_Modvol_CullMode));
		else
			gl_set_mvs_mode(Xor, PPW(param, isp, ISP_Modvol_CullMode));
		glDrawArrays(GL_TRIANGLES, mvFirst * 3, mvCount * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* sum the area */
			gl_set_mvs_mode(mv_mode == 1 ? Inclusion : Exclusion, PPW(param, isp, ISP_Modvol_CullMode));
			glDrawArrays(GL_TRIANGLES, mod_base * 3, (mvFirst + mvCount - mod_base) * 3);
			mod_base = -1;
		}
	}
	SetBaseClipping();
	/* culling off, colour written again */
	SetCull(0);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	SetupMainVBO(pvrrc.gpu, false);

	/* The polygons that take shadows, again, where they are in a volume.
	 * Of a frame decoded on the GPU, the volumes' bounds are the header
	 * pass's, from their blocks (TA_GPU_SHADOWS) */
	ga = pvrrc.gpu_shadow_area[render_pass < 10 ? render_pass : 0];
	if (pvrrc.gpu)
	{
		area.min_x = ga[0];
		area.min_y = ga[1];
		area.max_x = ga[2];
		area.max_y = ga[3];
	}
	else
		area = shadow_modvol_bounds(first, count);
	if (!screen_bounds_empty(&area))
	{
		int scissor[4];
		bounds_to_scissor(&area, &ViewportMatrix, scissor);
		glc_enable(GL_STENCIL_TEST);
		glc_stencil_mask(0x40);
		DrawShadowed(ListType_Opaque, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), previous_pass->op_count,
				current_pass->op_count - previous_pass->op_count, &area, scissor);
		DrawShadowed(ListType_Punch_Through, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), previous_pass->pt_count,
				current_pass->pt_count - previous_pass->pt_count, &area, scissor);
		SetCull(0);
		SetBaseClipping();
		glc_use_program(gl.modvol_shader.program);
	}

	/* black out any stencil with '1' */
	glc_enable(GL_BLEND);
	glc_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glc_enable(GL_STENCIL_TEST);
	/* only pixels that take shadows, are in a volume, and were not drawn again above */
	glc_stencil_func(GL_EQUAL, 0x81, 0xC1);

	/* the volume bits and the mark cleared */
	glc_stencil_mask(0x43);
	glc_stencil_op(GL_ZERO, GL_ZERO, GL_ZERO);

	/* no depth testing */
	glc_disable(GL_DEPTH_TEST);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* states restored */
	glc_enable(GL_DEPTH_TEST);
	glc_stencil_mask(0x3);
}

void DrawStrips(void)
{
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int npasses = TA_LIST_USED(pvrrc.render_passes, RenderPass);
	RenderPass previous_pass;
	int render_pass;

	/* (a frame decoded on the GPU has its sorted passes sorted already, and
	 * their indices went straight to the decoder's room for them when the
	 * decode was given: see TA_GPU_SORT) */
	SetupMainVBO(pvrrc.gpu, false);

	/* sampler 0 */
	glActiveTexture(GL_TEXTURE0);

	memset(&previous_pass, 0, sizeof(previous_pass));
	for (render_pass = 0; render_pass < npasses; render_pass++)
	{
		const RenderPass *current_pass = &passes[render_pass];

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d\n", render_pass + 1,
				current_pass->op_count - previous_pass.op_count,
				current_pass->pt_count - previous_pass.pt_count,
				current_pass->tr_count - previous_pass.tr_count,
				current_pass->mvo_count - previous_pass.mvo_count);

		/* initial state */
		glc_enable(GL_DEPTH_TEST);
		glc_depth_mask(GL_TRUE);

		/* opaque */
		DrawList(ListType_Opaque, false, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam),
				previous_pass.op_count, current_pass->op_count - previous_pass.op_count);

		/* alpha tested */
		DrawList(ListType_Punch_Through, false, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam),
				previous_pass.pt_count, current_pass->pt_count - previous_pass.pt_count);

		/* modifier volumes */
		if (gl.stencil_present)
			DrawModVols(previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count,
					&previous_pass, current_pass, render_pass);

		/* alpha blended */
		if (current_pass->autosort)
		{
			const bool lastPass = render_pass == npasses - 1;
			const int trFirst = previous_pass.tr_count;
			const int trCount = current_pass->tr_count - previous_pass.tr_count;
			if (pvrrc.gpu)
			{
				/* (a frame decoded on the GPU with a sorted pass was sorted by triangle: TA_GPU_SORT) */
				u32 nprepass = 0, nsorted = 0;
				const SortTrigDrawParam *prepass = ta_gpu_prepass(render_pass, &nprepass);
				const SortTrigDrawParam *sorted = ta_gpu_sorted(render_pass, &nsorted);
				if (!PrepassSupported())
					nprepass = 0;
				if (nprepass != 0 || nsorted != 0)
				{
					const uintptr_t base = (uintptr_t)gltad_bind_sorted();
					DrawPrepass(prepass, nprepass, base);
					DrawSorted(sorted, nsorted, !lastPass, base);
					gltad_bind_geometry();
				}
			}
			else if (settings.pvr.Emulation.AlphaSortMode == 0)
			{
				const uintptr_t base = SortTriangles(trFirst, trCount, true, lastPass);
				DrawPrepass(SortDraws(&prepassDraws), prepassDraws.count, base);
				DrawSorted(SortDraws(&sortedDraws), sortedDraws.count, !lastPass, base);
				/* (the main indices again, after sorted triangles or a prepass) */
				if (sortedDraws.count != 0 || prepassDraws.count != 0)
					glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_frame_index_buffer(gl.vbo.idxs));
			}
			else
			{
				const uintptr_t base = SortTriangles(trFirst, trCount, false, lastPass);
				if (prepassDraws.count != 0)
				{
					DrawPrepass(SortDraws(&prepassDraws), prepassDraws.count, base);
					glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_frame_index_buffer(gl.vbo.idxs));
				}
				SortPParams(trFirst, trCount);
				DrawList(ListType_Translucent, true, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), trFirst, trCount);
			}
		}
		else
			DrawList(ListType_Translucent, false, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam),
					previous_pass.tr_count, current_pass->tr_count - previous_pass.tr_count);

		previous_pass = *current_pass;
	}

	vertex_buffer_unmap();
}

/* A quad's four vertices, white, at depth @z, drawn as a strip of 0, 1, 2, 1, 3 */
static void QuadVertices(Vertex *v, float x, float y, float w, float h, float z,
		float u0, float v0, float u1, float v1)
{
	int i;
	memset(v, 0, 4 * sizeof(*v));
	for (i = 0; i < 4; i++)
	{
		v[i].x = (i & 2) ? x + w : x;
		v[i].y = (i & 1) ? y : y + h;
		v[i].z = z;
		v[i].col[0] = v[i].col[1] = v[i].col[2] = v[i].col[3] = 255;
		v[i].u = (i & 2) ? u1 : u0;
		v[i].v = (i & 1) ? v0 : v1;
	}
}

static const GLushort quad_indices[5] = { 0, 1, 2, 1, 3 };

/* The quad given to the renderer's own buffers and drawn */
static void DrawQuadVertices(const Vertex *vertices)
{
	SetupMainVBO(false, true);
	glBufferData(GL_ARRAY_BUFFER, 4 * sizeof(Vertex), vertices, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STREAM_DRAW);
	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);
}

static void DrawQuad(GLuint texId, float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
	Vertex vertices[4];
	PipelineShader *shader;

	QuadVertices(vertices, x, y, w, h, 0.1f, u0, v0, u1, v1);

	glc_disable(GL_SCISSOR_TEST);
	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);
	glc_disable(GL_CULL_FACE);
	glc_disable(GL_BLEND);

	ShaderUniforms.trilinear_alpha = 1.0;

	shader = GetProgram(false, false, true, false, true, 0, false, 2, false, false, false, false, false, false);
	UsePipelineShader(shader);

	glActiveTexture(GL_TEXTURE0);
	glc_bind_texture(GL_TEXTURE_2D, texId);

	DrawQuadVertices(vertices);
}

void DrawFramebuffer(void)
{
	/* (the texture is kept for the next: see RenderFramebuffer()) */
	DrawQuad(fbTextureId, 0, 0, 640.f, 480.f, 0, 0, 1, 1);
}

/* (the renderer's thread's: the pictures made into textures) */
static u8 vmu_pixels[VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH * 4];
static u8 crosshair_pixels[LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE * 4];

/* A small texture of its own, made the first time and bound */
static void BindOwnTexture(GLuint *id)
{
	if (*id == 0)
	{
		*id = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, *id);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
		glc_bind_texture(GL_TEXTURE_2D, *id);
}

void UpdateVmuTexture(int vmu_screen_number)
{
	const vmu_screen_params_t *p = &vmu_screen_params[vmu_screen_number];
	u8 *dst = vmu_pixels;
	const u8 *origsrc;
	s32 x, y;

	BindOwnTexture(&vmuTextureId[vmu_screen_number]);

	origsrc = vmu_lcd_pixels(vmu_screen_number);
	if (origsrc == NULL)
		return;

	for (y = VMU_SCREEN_HEIGHT - 1; y >= 0; y--)
	{
		const u8 *src = origsrc + (y * VMU_SCREEN_WIDTH);

		for (x = 0; x < VMU_SCREEN_WIDTH; x++)
		{
			if (*src++ > 0)
			{
				*dst++ = p->vmu_pixel_on_R;
				*dst++ = p->vmu_pixel_on_G;
				*dst++ = p->vmu_pixel_on_B;
			}
			else
			{
				*dst++ = p->vmu_pixel_off_R;
				*dst++ = p->vmu_pixel_off_G;
				*dst++ = p->vmu_pixel_off_B;
			}
			*dst++ = p->vmu_screen_opacity;
		}
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, vmu_pixels);

	vmu_screen_params[vmu_screen_number].vmu_screen_needs_update = false;
}

void DrawVmuTexture(u8 vmu_screen_number)
{
	const float w = VMU_SCREEN_WIDTH * vmu_screen_params[vmu_screen_number].vmu_screen_size_mult;
	const float h = VMU_SCREEN_HEIGHT * vmu_screen_params[vmu_screen_number].vmu_screen_size_mult;
	float x = 0, y = 0;
	Vertex vertices[4];
	PipelineShader *shader;
	/* A new picture from the game, new colours from the options, or no texture yet */
	const bool vmu_lcd_changed = vmu_lcd_refresh(vmu_screen_number);

	glActiveTexture(GL_TEXTURE0);

	if (vmu_lcd_changed || vmu_screen_params[vmu_screen_number].vmu_screen_needs_update || vmuTextureId[vmu_screen_number] == 0)
		UpdateVmuTexture(vmu_screen_number);

	switch (vmu_screen_params[vmu_screen_number].vmu_screen_position)
	{
	case UPPER_LEFT:
		break;
	case UPPER_RIGHT:
		x = 640 - w;
		break;
	case LOWER_LEFT:
		y = 480 - h;
		break;
	case LOWER_RIGHT:
		x = 640 - w;
		y = 480 - h;
		break;
	}

	glc_bind_texture(GL_TEXTURE_2D, vmuTextureId[vmu_screen_number]);

	glc_disable(GL_SCISSOR_TEST);
	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);
	glc_disable(GL_CULL_FACE);
	glc_enable(GL_BLEND);
	glc_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	shader = GetProgram(0, false, 1, 1, 0, 0, 0, 2, false, false, false, false, false, false);
	UsePipelineShader(shader);

	QuadVertices(vertices, x, y, w, h, 1.f, 0, 0, 1, 1);
	DrawQuadVertices(vertices);
}

static void UpdateLightGunTexture(int port)
{
	const u8 *colour = &lightgun_palette[lightgun_params[port].colour * 3];
	u8 *dst = crosshair_pixels;
	s32 x, y;

	BindOwnTexture(&lightgunTextureId[port]);

	for (y = LIGHTGUN_CROSSHAIR_SIZE - 1; y >= 0; y--)
	{
		const u8 *src = lightgun_img_crosshair + (y * LIGHTGUN_CROSSHAIR_SIZE);

		for (x = 0; x < LIGHTGUN_CROSSHAIR_SIZE; x++)
		{
			if (src[x])
			{
				*dst++ = colour[0];
				*dst++ = colour[1];
				*dst++ = colour[2];
				*dst++ = 0xFF;
			}
			else
			{
				*dst++ = 0;
				*dst++ = 0;
				*dst++ = 0;
				*dst++ = 0;
			}
		}
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, crosshair_pixels);

	lightgun_params[port].dirty = false;
}

void DrawGunCrosshair(u8 port)
{
	const float w = LIGHTGUN_CROSSHAIR_SIZE;
	const float h = LIGHTGUN_CROSSHAIR_SIZE;
	float x, y;
	Vertex vertices[4];
	PipelineShader *shader;

	if (lightgun_params[port].offscreen || lightgun_params[port].colour == 0)
		return;

	glActiveTexture(GL_TEXTURE0);

	x = lightgun_params[port].x - (LIGHTGUN_CROSSHAIR_SIZE / 2);
	y = lightgun_params[port].y - (LIGHTGUN_CROSSHAIR_SIZE / 2);

	if (lightgun_params[port].dirty || lightgunTextureId[port] == 0)
		UpdateLightGunTexture(port);

	glc_bind_texture(GL_TEXTURE_2D, lightgunTextureId[port]);

	glc_disable(GL_SCISSOR_TEST);
	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);
	glc_disable(GL_CULL_FACE);
	glc_enable(GL_BLEND);
	glc_blend_func(GL_SRC_ALPHA, GL_ONE);

	shader = GetProgram(0, false, 1, 1, 0, 0, 0, 2, false, false, false, false, false, false);
	UsePipelineShader(shader);

	QuadVertices(vertices, x, y, w, h, 1.f, 0, 0, 1, 1);
	DrawQuadVertices(vertices);

	glc_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
