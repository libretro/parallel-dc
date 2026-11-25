#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/*
    Created on: Oct 2, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <stdlib.h>
#include <string.h>
#include "vk_renderer.h"
#include "vk_screen_drawer.h"
#include "vk_tex_decoder.h"
#include "vk_ta_decoder.h"
#include "pso.h"
#include "rtt_read.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta.h"
#include "rend/TexCache.h"
#include "rend/texconv.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include "rend/upscaler.h"
#include "rend/ssaa_pattern.h"

void vk_renderer_construct(vk_renderer *r, const vk_renderer_kind *kind, void *self)
{
	memset(r, 0, sizeof(*r));
	r->kind = kind;
	r->self = self;
	r->samples = 1;
	vk_tex_decoder_construct(&r->texDecoder);
	vk_tex_pool_construct(&r->texPool);
	vk_ta_decoder_construct(&r->taDecoder);
	vk_texcache_construct(&r->textureCache);
	vk_quad_pipeline_construct(&r->quadPipeline);
	vk_quad_pipeline_construct(&r->overlayQuadPipeline);
	r->parsedSlot = -1;
}

static vk_texture *texture_new(int type)
{
	vk_texture *t = (vk_texture *)malloc(sizeof(vk_texture));
	if (t == NULL)
		return NULL;
	vk_texture_construct(t);
	t->tc.tex_type = (u8)type;
	return t;
}

static void texture_delete(vk_texture **t)
{
	if (*t == NULL)
		return;
	vk_texture_destroy(*t);
	free(*t);
	*t = NULL;
}

static void framebuffer_textures_free(vk_renderer *r)
{
	u32 i;
	for (i = 0; i < r->framebufferTextureCount; i++)
		texture_delete(&r->framebufferTextures[i]);
	free(r->framebufferTextures);
	r->framebufferTextures = NULL;
	r->framebufferTextureCount = 0;
}

static void osd_free(vk_renderer *r)
{
	if (r->osdMade)
		vk_osd_term(&r->osd);
	r->osdMade = false;
}

void vk_renderer_free(vk_renderer *r)
{
	osd_free(r);
	if (r->parseArenaOk)
		parse_arena_vk_term(&r->parseArena);
	r->parseArenaOk = false;
	vk_quad_pipeline_term(&r->overlayQuadPipeline);
	vk_quad_pipeline_term(&r->quadPipeline);
	texture_delete(&r->keptPicture);
	vk_texcache_free(&r->textureCache);
	vk_ta_decoder_term(&r->taDecoder);
	vk_tex_pool_term(&r->texPool);
	vk_tex_decoder_term(&r->texDecoder);
	framebuffer_textures_free(r);
	free(r->framebufferPixels);
	r->framebufferPixels = NULL;
	r->framebufferBytes = 0;
	vk_cmd_pool_term(&r->texCommandPool);
	texture_delete(&r->paletteTexture);
	texture_delete(&r->fogTexture);
	vk_raster_modules_term(&r->modules);
}

static void detach_parse(vk_renderer *r)
{
	if (r->parsedSlot >= 0)
		parse_arena_detach(&r->parseArena.arena);
	r->parsedSlot = -1;
}

void vk_renderer_drop_quads(vk_renderer *r)
{
	vk_quad_pipeline_forget(&r->quadPipeline);
	vk_quad_pipeline_forget(&r->overlayQuadPipeline);
}

void vk_renderer_term(vk_renderer *r)
{
	ta_set_flat_strips(false);
	ta_set_modvol_clip(false);
	/* (the quad pipelines before the modules they may be being made with) */
	vk_quad_pipeline_term(&r->quadPipeline);
	vk_quad_pipeline_term(&r->overlayQuadPipeline);
	vk_raster_modules_term(&r->modules);
	osd_free(r);
	vk_rtt_term();
	vk_texcache_clear(&r->textureCache);
	vk_texture_decoder = NULL;
	vk_tex_decoder_term(&r->texDecoder);
	vk_ta_decoder_term(&r->taDecoder);
	/* (the renderers waited for the card's frames) */
	if (r->parseArenaOk)
	{
		NOTICE_LOG(RENDERER, "Vulkan: %u renders parsed where the card reads them, %u copied there\n",
				r->parsedHere, r->parsedCopied);
		parse_arena_vk_term(&r->parseArena);
		r->parseArenaOk = false;
	}
	texture_delete(&r->keptPicture);
	texture_delete(&r->fogTexture);
	texture_delete(&r->paletteTexture);
	vk_cmd_pool_term(&r->texCommandPool);
	framebuffer_textures_free(r);
	/* (what the cache's textures had is back in the pool by now) */
	vk_texture_pool = NULL;
	vk_tex_pool_term(&r->texPool);
}

bool vk_renderer_base_init(vk_renderer *r, VkRenderPass renderPass, int subpass)
{
	size_t room[PARSE_ARENA_PARTS];
	u64 ring;
	if (!vk_cmd_pool_init(&r->texCommandPool))
		return false;
	vk_rtt_init();
	/* where the frame's lists are parsed into, one slice a frame in
	 * flight, where the card can read them as they are: memory the CPU
	 * reads back fast too, as the parse and the sort do (a machine
	 * whose video memory is the CPU's) - or none, and they are copied.
	 * (Made once: a change of size comes back here, the frames on the
	 * card done, and keeps it while it has as many slices as there are
	 * slots, of the machine's lists' room.) */
	tactx_arena_rooms(room);
	if (r->parseArenaOk && (r->parseArena.arena.slices != (unsigned)r->texCommandPool.slotCount
			|| memcmp(r->parseArena.arena.room, room, sizeof(room)) != 0))
	{
		parse_arena_vk_term(&r->parseArena);
		r->parseArenaOk = false;
	}
	if (!r->parseArenaOk)
	{
		r->parseArenaOk = parse_arena_vk_init(&r->parseArena, vkc.device, &vkc.heap, room, (unsigned)r->texCommandPool.slotCount) != 0;
		r->parsedHere = r->parsedCopied = 0;
	}
	detach_parse(r);
	/* where the textures converted on the CPU go to the GPU through: one
	 * buffer, a share of the video memory, between 16 and 64 MB */
	ring = (u64)vram_budget_available_kib(&vkc.vramBudget) * 1024 / 32;
	if (ring < (16ull << 20))
		ring = 16ull << 20;
	if (ring > (64ull << 20))
		ring = 64ull << 20;
	if (!vk_upload_ring_init(vk_upload_ring_get(), (size_t)ring, &r->texCommandPool))
		return false;
	vk_tex_decoder_init(&r->texDecoder);
	vk_texture_decoder = &r->texDecoder;
	/* the textures' images, all of them now (rend/tex_pool.h) */
	vk_tex_pool_init(&r->texPool, &r->texDecoder);
	vk_texture_pool = &r->texPool;
	/* ...and the cache's room for them (rend/tex_map.h) */
	vk_texcache_reserve_for(&r->textureCache, r->texPool.book.made);
	if (!vk_quad_pipeline_init(&r->quadPipeline, &r->modules, renderPass, subpass, 1))
		return false;
	osd_free(r);
	vk_osd_construct(&r->osd);
	r->osdMade = true;
	if (!vk_osd_init(&r->osd, &r->quadPipeline))
		return false;
	/* GPU-side TA processing, if it is on: here, not in a frame */
	vk_ta_decoder_setup(&r->taDecoder, &r->texCommandPool, ta_gpu_wanted());
	return true;
}

bool vk_renderer_init_overlay(vk_renderer *r, VkRenderPass pass)
{
	vk_quad_pipeline_forget(&r->overlayQuadPipeline);
	return pass == VK_NULL_HANDLE || vk_quad_pipeline_init(&r->overlayQuadPipeline, &r->modules, pass, 0, 1);
}

static void begin_frame(vk_renderer *r)
{
	vk_cmd_pool_begin_frame(&r->texCommandPool);
	vk_texcache_set_current_index(&r->textureCache, r->texCommandPool.index);
	vk_tex_decoder_begin_frame(&r->texDecoder, r->texCommandPool.index);
	vk_ta_decoder_begin_frame(&r->taDecoder, &r->texCommandPool);
}

/* @t (@width x @height) uploaded from @pixels in a command buffer of its own */
static void upload(vk_renderer *r, vk_texture *t, int width, int height, u8 *pixels)
{
	t->commandBuffer = vk_cmd_pool_allocate(&r->texCommandPool);
	vk_texture_upload_to_gpu(t, width, height, pixels, false, false);
	t->commandBuffer = VK_NULL_HANDLE;
}

void vk_renderer_present(vk_cmd_pool *pool, VkImage image, VkImageView view, u32 width, u32 height, s32 extentX, s32 extentY,
		VkImageView sums)
{
	bool hdr = false;
	if (vkc.hdr != NULL)
	{
		VkCommandBuffer cmd = vk_cmd_pool_allocate(pool);
		VkCommandBufferBeginInfo begin;
		VkImage i = image;
		VkImageView v = view;
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
		hdr = vkc_hdr_encode(pool, (unsigned)pool->index, (unsigned)pool->frames, cmd, &i, &v, width, height, sums);
		vkEndCommandBuffer(cmd);
		if (hdr)
		{
			image = i;
			view = v;
		}
	}
	vk_cmd_pool_end_frame(pool);
	vkc_present_frame(image, view, extentX, extentY, hdr);
}

static void present_texture(vk_renderer *r, vk_texture *t, int width, int height, s32 extentX, s32 extentY)
{
	vk_renderer_present(&r->texCommandPool, vk_texture_image(t), vk_texture_view(t), (u32)width, (u32)height, extentX, extentY,
			VK_NULL_HANDLE);
}

/* the texture a picture of the CPU's is handed to the frontend in, for the frontend's frame index */
static vk_texture *framebuffer_texture(vk_renderer *r)
{
	const u32 imageIndex = (u32)vkc_current_image_index();
	vk_texture **t;
	if (r->framebufferTextureCount <= imageIndex)
	{
		u32 count = (u32)vkc.swapChainSize > imageIndex + 1 ? (u32)vkc.swapChainSize : imageIndex + 1;
		vk_texture **more = (vk_texture **)realloc(r->framebufferTextures, count * sizeof(vk_texture *));
		if (more == NULL)
			return NULL;
		memset(&more[r->framebufferTextureCount], 0, (count - r->framebufferTextureCount) * sizeof(vk_texture *));
		r->framebufferTextures = more;
		r->framebufferTextureCount = count;
	}
	t = &r->framebufferTextures[imageIndex];
	if (*t == NULL)
	{
		*t = texture_new(TEX_TYPE_8888);
		if (*t != NULL)
			(*t)->presented = true;
	}
	return *t;
}

bool vk_renderer_present_kept_picture(vk_renderer *r)
{
	int width, height;
	const u8 *pixels = last_picture(&width, &height);
	vk_texture *t;
	if (pixels == NULL)
		return false;
	/* (as vk_renderer_process() begins a frame) */
	begin_frame(r);
	t = framebuffer_texture(r);
	if (t == NULL)
	{
		vk_cmd_pool_end_frame(&r->texCommandPool);
		return false;
	}
	upload(r, t, width, height, (u8 *)pixels);
	present_texture(r, t, width, height, width, height);
	return true;
}

void vk_renderer_options_changed(vk_renderer *r)
{
	/* (TA processing switched on or off) */
	vk_ta_decoder_setup(&r->taDecoder, &r->texCommandPool, ta_gpu_wanted());
}

u64 vk_renderer_get_texture(vk_renderer *r, u32 tsp, u32 tcw, int area)
{
	vk_texture *tf = vk_texcache_get(&r->textureCache, tsp, tcw, area);
	if (vk_texture_is_new(tf))
	{
		texcache_create(&tf->tc);
		tf->cached = true;
	}
	/* updated if needed */
	if (texcache_needs_update(&tf->tc))
	{
		/* a command buffer of its own if it is uploaded from here; one
		 * made on the GPU is recorded with the others (vk_renderer_process()) */
		tf->commandPool = &r->texCommandPool;
		texcache_update(&tf->tc);
	}
	else if (texcache_custom_available(&tf->tc))
	{
		vk_texcache_destroy_later(&r->textureCache, tf);
		tf->commandPool = &r->texCommandPool;
		texcache_check_custom(&tf->tc);
	}
	tf->commandBuffer = VK_NULL_HANDLE;
	tf->commandPool = NULL;
	vk_texcache_set_in_flight(&r->textureCache, tf);
	return (u64)(uintptr_t)tf;
}

/* In a new context, the picture kept from the old one (rend/last_picture.h)
 * is made a texture, and the screen's drawer starts its first frame from it. */
static void check_kept_picture(vk_renderer *r)
{
	int width, height;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == NULL)
		return;
	texture_delete(&r->keptPicture);
	r->keptPicture = texture_new(TEX_TYPE_8888);
	if (r->keptPicture == NULL)
		return;
	upload(r, r->keptPicture, width, height, (u8 *)pixels);
	r->kind->set_restored_picture(r->self, vk_texture_view(r->keptPicture), (u32)width, (u32)height);
}

static bool render_framebuffer(vk_renderer *r)
{
	FramebufferRead fb;
	vk_texture *t;
	u8 *pixels;
	size_t bytes;
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0 || (t = framebuffer_texture(r)) == NULL)
	{
		/* The frame begun on the texture command pool reset its fence;
		 * only a submit signals it again. Leave without one and the pool's
		 * begin waits on that fence for ever when it comes round to this
		 * slot. */
		vk_cmd_pool_end_frame(&r->texCommandPool);
		return false;
	}
	ReadFramebufferGeometry(&fb);
	bytes = (size_t)fb.width * fb.height * 4;
	t->commandBuffer = vk_cmd_pool_allocate(&r->texCommandPool);
	/* converted where the copy to the image reads it: the upload ring */
	pixels = (u8 *)vk_texture_upload_memory(t, (u32)fb.width, (u32)fb.height, bytes, 4, false);
	if (pixels == NULL)
	{
		/* (no room in the ring, or an image written where it is) */
		if (r->framebufferBytes < bytes)
		{
			u8 *more = (u8 *)realloc(r->framebufferPixels, bytes);
			if (more == NULL)
			{
				t->commandBuffer = VK_NULL_HANDLE;
				vk_cmd_pool_end_frame(&r->texCommandPool);
				return false;
			}
			r->framebufferPixels = more;
			r->framebufferBytes = bytes;
		}
		pixels = r->framebufferPixels;
	}
	ReadFramebufferTo(&fb, pixels, (size_t)fb.width * 4);
	vk_texture_upload_to_gpu(t, fb.width, fb.height, pixels, false, false);
	t->commandBuffer = VK_NULL_HANDLE;
	present_texture(r, t, fb.width, fb.height, fb.width, fb.height);
	return true;
}

static bool check_fog_texture(vk_renderer *r)
{
	u8 texData[256];
	if (r->fogTexture == NULL)
	{
		r->fogTexture = texture_new(TEX_TYPE_8);
		if (r->fogTexture == NULL)
			return false;
		r->fogUploadedValid = false;
	}
	/* Uploaded when it differs from what the GPU has: see UpdateFogTexture() in gles/gles.c */
	MakeFogTexture(texData);
	if (r->fogUploadedValid && !memcmp(r->fogUploaded, texData, sizeof(r->fogUploaded)))
		return true;
	memcpy(r->fogUploaded, texData, sizeof(r->fogUploaded));
	r->fogUploadedValid = true;
	upload(r, r->fogTexture, 128, 2, texData);
	return true;
}

static bool check_palette_texture(vk_renderer *r)
{
	if (r->paletteTexture == NULL)
	{
		r->paletteTexture = texture_new(TEX_TYPE_8888);
		if (r->paletteTexture == NULL)
			return false;
		palette_updated = true;
	}
	if (!palette_updated)
		return true;
	palette_updated = false;
	upload(r, r->paletteTexture, 1024, 1, (u8 *)palette32_ram);
	return true;
}

bool vk_renderer_process(vk_renderer *r, TA_context *ctx)
{
	bool result;
	begin_frame(r);

	if (!ctx->rend.isRTT)
		vk_osd_prepare(&r->osd, &r->texCommandPool);
	if (!ctx->rend.isRTT && !ctx->rend.isRenderFramebuffer)
		check_kept_picture(r);

	if (ctx->rend.isRenderFramebuffer)
		return render_framebuffer(r);

	if (KillTex)
	{
		/* what was rendered to a texture and is not in video memory yet goes there before its picture does */
		rtt_watch_flush();
		vk_texcache_clear_later(&r->textureCache);
	}

	/* SSAA draws flat strips at a single rate: the parse and the GPU's
	 * decode's header pass look for them then, and only then */
	ta_set_flat_strips(r->samples > 1);
	/* the modifier volumes' triangles far off the screen cut before the
	 * GPU clips them (rend/modvol_clip.h) */
	ta_set_modvol_clip(true);
	/* GPU-side TA processing: the header pass makes the frame ready to
	 * be drawn from the GPU's decode, in the full parse's place; or it
	 * is parsed as ever (the upscaler's motion vectors are matched from
	 * the parse's strips: rend/upscaler.h) */
	if (!r->taDecoder.ready || upscaler_active() || !vk_ta_decoder_prepare(&r->taDecoder, ctx, r->kind->ta_flags(r->self), &result))
	{
		/* into this frame's slice of the memory the card reads, where
		 * there is one: the draws read it there, uncopied (with the
		 * background polygon's four vertices FillBGP() wrote, at the
		 * head of the list the parse empties and takes them back in) */
		ta_list_t *lists[PARSE_ARENA_PARTS];
		tactx_arena_lists(ctx, lists);
		if (r->parseArenaOk && parse_arena_attach(&r->parseArena.arena, (unsigned)r->texCommandPool.index,
				lists, 4 * sizeof(Vertex)))
			r->parsedSlot = r->texCommandPool.index;
		result = ta_parse_vdrc(ctx);
		if (!result)
			detach_parse(r);
	}
	else if (result && !vk_ta_decoder_record(&r->taDecoder, ctx, &r->texCommandPool))
		result = false;
	/* the textures the frame's polygons asked for that are made on the GPU, before the draws */
	if (!vk_tex_decoder_flush(&r->texDecoder, &r->texCommandPool))
		result = false;

	vk_texcache_cleanup(&r->textureCache);

	if (result)
		result = check_fog_texture(r) && check_palette_texture(r);
	if (!result)
	{
		detach_parse(r);
		vk_cmd_pool_end_frame(&r->texCommandPool);
	}
	return result;
}

bool vk_renderer_render(vk_renderer *r)
{
	const bool rv = r->kind->render_frame(r->self);
	detach_parse(r);
	return rv;
}

void vk_renderer_set_parsed(vk_renderer *r, vk_base_drawer *drawer)
{
	vk_parsed parsed;
	memset(&parsed, 0, sizeof(parsed));
	if (r->parsedSlot >= 0 && r->parsedSlot == r->texCommandPool.index)
	{
		const parse_arena_t *a = &r->parseArena.arena;
		parsed.buffer = r->parseArena.buffer;
		parsed.vertices = parse_arena_offset(a, (unsigned)r->parsedSlot, PARSE_ARENA_VERTS);
		parsed.indices = parse_arena_offset(a, (unsigned)r->parsedSlot, PARSE_ARENA_IDX);
		parsed.sortRoom = (u32)(a->space[PARSE_ARENA_IDX] - TA_LIST_BYTES(pvrrc.idx));
		/* (the volumes' triangles drawn: those cut where there are, rend/modvol_clip.h) */
		parsed.modVols = parse_arena_offset(a, (unsigned)r->parsedSlot,
				TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0 ? PARSE_ARENA_MV_CLIP : PARSE_ARENA_MODTRIG);
		if (r->parsedHere++ == 0)
			NOTICE_LOG(RENDERER, "Vulkan: renders parsed where the card reads them, not copied there\n");
	}
	else if (!pvrrc.gpu && r->parsedCopied++ == 0 && r->parseArenaOk)
		WARN_LOG(RENDERER, "Vulkan: a render copied to the card though its memory has the parse's room\n");
	drawer->parsed = parsed;
}

/* ---- The per-triangle renderer ---- */

typedef struct vk_tri_renderer
{
	vk_renderer r;
	vk_samplers samplers;
	vk_screen_drawer screen;
	vk_rtt_drawer rtt;
} vk_tri_renderer;

static vk_tri_renderer *tri;

/* SSAA's samples a pixel, for the screen: what the option asks, as far as
 * the device and the video memory budget go for its attachments (the
 * upscaler is the anti-aliasing then, and reads the depth, which SSAA's
 * samples are not) */
static u32 tri_ssaa_samples(void)
{
	if (upscaler_active())
		return 1;
	return vkc_ssaa_samples((u32)settings.rend.Ssaa, ssaa_per_sample_kib(vkc.width, vkc.height, 4 + vkc_depth_bytes()),
			0, true, "Vulkan per-triangle renderer");
}

/* ...and for the textures rendered to: the same where their sample
 * attachments cost nothing (transient, never stored, in memory that is
 * only backed when used); one elsewhere, since they are made as large as
 * such a texture can be, which is more video memory than the screen's */
static u32 tri_rtt_samples(void)
{
	return vkc.lazyMemory ? tri->r.samples : 1;
}

static bool tri_init_texture_drawer(void)
{
	return vk_rtt_drawer_init(&tri->rtt, &tri->samplers, &tri->r.modules, &tri->r.textureCache, tri_rtt_samples());
}

/* (and the textures' depth, shared with it, and what draws over it) */
static bool tri_init_screen_drawer(void)
{
	return vk_screen_drawer_init(&tri->screen, &tri->samplers, &tri->r.modules, tri->r.samples)
			&& vk_rtt_drawer_share_depth(&tri->rtt, &tri->screen)
			&& vk_renderer_base_init(&tri->r, tri->screen.renderPass, vk_screen_drawer_osd_subpass(&tri->screen))
			&& vk_renderer_init_overlay(&tri->r, tri->screen.writeOut.overlayPass)
			&& vk_screen_drawer_set_quad_pipeline(&tri->screen, &tri->r.quadPipeline);
}

/* A sorted pass's translucent triangles are sorted from the frame's
 * depths (TA_GPU_SORT); sorted by strip, the polygons are sorted from the
 * CPU's vertices, and a frame that may have a sorted pass is parsed. The
 * polygons drawn again for shadows are those the GPU says reach into the
 * volumes. */
static u32 tri_ta_flags(void *self)
{
	(void)self;
	return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_SHADOWS
			| (!settings.pvr.Emulation.AlphaSortMode && tri->r.taDecoder.sortable ? TA_GPU_SORT : TA_GPU_NO_SORT);
}

static void tri_set_restored_picture(void *self, VkImageView view, u32 width, u32 height)
{
	VkExtent2D size;
	(void)self;
	size.width = width;
	size.height = height;
	vk_screen_drawer_set_restored_picture(&tri->screen, view, size);
}

static bool tri_render_frame(void *self)
{
	vk_renderer *r = &tri->r;
	VkImageView fogView, paletteView;
	bool after;
	(void)self;
	if (pvrrc.isRenderFramebuffer)
		return true;
	if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		/* A pixel clip that ends before it starts holds no pixel: the
		 * chip writes nothing. (Its size would be negative here.) */
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		return false;
	}
	fogView = vk_texture_view(r->fogTexture);
	paletteView = vk_texture_view(r->paletteTexture);
	if (pvrrc.isRTT)
	{
		vk_renderer_set_parsed(r, &tri->rtt.base);
		if (!vk_rtt_drawer_draw(&tri->rtt, fogView, paletteView))
			ERROR_LOG(RENDERER, "Vulkan: a render to a texture could not be drawn\n");
		if (tri->rtt.cmd != VK_NULL_HANDLE)
			vk_rtt_drawer_end(&tri->rtt);
		return false;
	}

	vk_renderer_set_parsed(r, &tri->screen.base);
	if (!vk_screen_drawer_draw(&tri->screen, fogView, paletteView))
		ERROR_LOG(RENDERER, "Vulkan: the frame could not be drawn\n");
	if (tri->screen.cmd == VK_NULL_HANDLE)
	{
		vk_cmd_pool_end_frame(&r->texCommandPool);
		return false;
	}
	/* (the chip's write-out before what goes over the picture, which then
	 * goes in a render pass of its own: writeout.h) */
	after = vk_screen_drawer_has_writeout(&tri->screen) && vk_osd_drawing(&r->osd) && vk_screen_drawer_writeout_before_osd(&tri->screen);
	if (!vk_osd_draw(&r->osd, tri->screen.cmd, (u32)screen_width, (u32)screen_height, &r->texCommandPool,
			after ? &r->overlayQuadPipeline : NULL))
		ERROR_LOG(RENDERER, "Vulkan: the OSD's pipeline could not be made\n");
	if (!vk_screen_drawer_end(&tri->screen))
		ERROR_LOG(RENDERER, "Vulkan: the frame's motion vectors could not be drawn\n");
	return true;
}

static const vk_renderer_kind tri_kind = { tri_render_frame, tri_ta_flags, tri_set_restored_picture };

static void tri_create(void)
{
	tri = (vk_tri_renderer *)calloc(1, sizeof(*tri));
	if (tri == NULL)
		return;
	vk_renderer_construct(&tri->r, &tri_kind, tri);
	vk_screen_drawer_construct(&tri->screen);
	vk_rtt_drawer_construct(&tri->rtt);
}

static void tri_release(void)
{
	if (tri == NULL)
		return;
	vk_rtt_drawer_free(&tri->rtt);
	vk_screen_drawer_free(&tri->screen);
	vk_samplers_term(&tri->samplers);
	vk_renderer_free(&tri->r);
	free(tri);
	tri = NULL;
}

static bool tri_init(void)
{
	DEBUG_LOG(RENDERER, "VulkanRenderer::Init\n");
	if (tri == NULL)
		return false;
	tri->r.samples = tri_ssaa_samples();
	vk_samplers_init(&tri->samplers);
	vk_rtt_drawer_set_pool(&tri->rtt, &tri->r.texCommandPool);
	vk_screen_drawer_set_pool(&tri->screen, &tri->r.texCommandPool);
	tri->rtt.base.gpuDraw = &tri->r.taDecoder.draw;
	tri->screen.base.gpuDraw = &tri->r.taDecoder.draw;
	if (!tri_init_texture_drawer() || !tri_init_screen_drawer())
		return false;
	tri->r.taDecoder.sortWanted = true;
	return true;
}

static void tri_resize(int w, int h)
{
	const u32 was = tri->r.samples;
	(void)w;
	(void)h;
	/* what is replaced below may be in use by frames still on the GPU */
	vk_cmd_pool_wait_all(&tri->r.texCommandPool);
	tri->r.samples = tri_ssaa_samples();
	if (tri->r.samples != was)
	{
		if (!tri_init_texture_drawer())
			ERROR_LOG(RENDERER, "Vulkan: the texture drawer could not be made again\n");
		vk_quad_pipeline_forget(&tri->r.quadPipeline);
	}
	if (!tri_init_screen_drawer())
		ERROR_LOG(RENDERER, "Vulkan: the screen drawer could not be made again\n");
	/* what this size no longer needs is given back once the frames that used it are done */
	vkc_heap_trim_soon();
}

static void tri_term(void)
{
	DEBUG_LOG(RENDERER, "VulkanRenderer::Term\n");
	vkc_log_vram_usage();
	/* (the frontend lets go of the pictures before they go) */
	vkc_release_picture();
	vk_cmd_pool_wait_all(&tri->r.texCommandPool);
	vk_renderer_drop_quads(&tri->r);
	vk_screen_drawer_term(&tri->screen);
	vk_rtt_drawer_term(&tri->rtt);
	vk_samplers_term(&tri->samplers);
	vk_renderer_term(&tri->r);
}

static bool tri_process(TA_context *ctx) { return vk_renderer_process(&tri->r, ctx); }
static bool tri_render(void) { return vk_renderer_render(&tri->r); }
static void tri_keep_picture(void) { vk_screen_drawer_keep_picture(&tri->screen); }
static u64 tri_get_texture(u32 tsp, u32 tcw, int area) { return vk_renderer_get_texture(&tri->r, tsp, tcw, area); }
static void tri_options_changed(void) { vk_renderer_options_changed(&tri->r); }
static void tri_present(void) { }
static bool tri_present_kept_picture(void) { return vk_renderer_present_kept_picture(&tri->r); }
static bool tri_ready(void) { return vk_pso_worker_batch_ready(); }

static const renderer_ops tri_ops =
{
	tri_init,
	tri_resize,
	tri_term,
	tri_process,
	tri_render,
	tri_keep_picture,
	tri_get_texture,
	tri_options_changed,
	tri_create,
	tri_release,
	tri_present,
	tri_present_kept_picture,
	tri_ready
};

const renderer_ops *rend_Vulkan(void)
{
	return &tri_ops;
}
