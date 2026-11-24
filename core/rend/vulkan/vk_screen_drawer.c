#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/*
	Created on: Oct 8, 2019

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
#include "vk_screen_drawer.h"
#include "rtt_read.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/ta_words.h"
#include "rend/TexCache.h"
#include "rend/last_picture.h"
#include "rend/upscaler.h"

/* ---- What goes once the frames that may use it are done ---- */

static void attachment_gone(void *object)
{
	vk_attachment_free((vk_attachment *)object);
	free(object);
}

/* @a goes once the frames of @pool that may use it are done (now with no
 * pool), and is left empty */
static void defer_attachment(vk_cmd_pool *pool, vk_attachment *a)
{
	vk_attachment *kept;
	if (a->image == VK_NULL_HANDLE && a->view == VK_NULL_HANDLE)
	{
		vk_attachment_free(a);
		memset(a, 0, sizeof(*a));
		return;
	}
	kept = pool != NULL ? (vk_attachment *)malloc(sizeof(*kept)) : NULL;
	if (kept != NULL)
	{
		*kept = *a;
		vk_cmd_pool_defer(pool, kept, attachment_gone);
	}
	else
		vk_attachment_free(a);
	memset(a, 0, sizeof(*a));
}

typedef struct framebuffer_box
{
	VkFramebuffer framebuffer;
} framebuffer_box;

static void framebuffer_gone(void *object)
{
	vkDestroyFramebuffer(vkc.device, ((framebuffer_box *)object)->framebuffer, NULL);
	free(object);
}

static void defer_framebuffer(vk_cmd_pool *pool, VkFramebuffer *framebuffer)
{
	framebuffer_box *kept;
	if (*framebuffer == VK_NULL_HANDLE)
		return;
	kept = pool != NULL ? (framebuffer_box *)malloc(sizeof(*kept)) : NULL;
	if (kept != NULL)
	{
		kept->framebuffer = *framebuffer;
		vk_cmd_pool_defer(pool, kept, framebuffer_gone);
	}
	else
		vkDestroyFramebuffer(vkc.device, *framebuffer, NULL);
	*framebuffer = VK_NULL_HANDLE;
}

static VkSubpassDependency *dep(VkSubpassDependency *d, u32 src, u32 dst, VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
		VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkDependencyFlags flags)
{
	d->srcSubpass = src;
	d->dstSubpass = dst;
	d->srcStageMask = srcStages;
	d->dstStageMask = dstStages;
	d->srcAccessMask = srcAccess;
	d->dstAccessMask = dstAccess;
	d->dependencyFlags = flags;
	return d + 1;
}

static void attachment_description(VkAttachmentDescription *a, VkFormat format, VkAttachmentLoadOp load, VkAttachmentStoreOp store,
		VkAttachmentLoadOp stencilLoad, VkImageLayout finalLayout)
{
	memset(a, 0, sizeof(*a));
	a->format = format;
	a->samples = VK_SAMPLE_COUNT_1_BIT;
	a->loadOp = load;
	a->storeOp = store;
	a->stencilLoadOp = stencilLoad;
	a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	a->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	a->finalLayout = finalLayout;
}

/* ---- The screen's ---- */

void vk_screen_drawer_construct(vk_screen_drawer *d)
{
	memset(d, 0, sizeof(*d));
	vk_base_drawer_construct(&d->base);
	vk_tri_drawer_construct(&d->tri, &d->base);
	vk_pipeline_mgr_construct(&d->pipelines, 1);
	vk_ssaa_resolve_construct(&d->ssaaResolve);
	vk_writeout_construct(&d->writeOut);
	vk_quad_pipeline_construct(&d->msQuadPipeline);
	vk_motion_pass_construct(&d->motionPass);
	d->timer.what = "";
	d->samples = 1;
}

/* each image's things beyond @count gone now (none of them is used: the
 * frames on the GPU were waited for) */
static void screen_images_drop(vk_screen_drawer *d, u32 count)
{
	while (d->images > count)
	{
		d->images--;
		if (d->framebuffers[d->images] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(vkc.device, d->framebuffers[d->images], NULL);
		d->framebuffers[d->images] = VK_NULL_HANDLE;
		vk_attachment_free(&d->colorAttachments[d->images]);
		vk_attachment_free(&d->hdrAttachments[d->images]);
		vk_attachment_free(&d->depthAttachments[d->images]);
		memset(&d->colorAttachments[d->images], 0, sizeof(vk_attachment));
		memset(&d->hdrAttachments[d->images], 0, sizeof(vk_attachment));
		memset(&d->depthAttachments[d->images], 0, sizeof(vk_attachment));
	}
}

void vk_screen_drawer_term(vk_screen_drawer *d)
{
	vk_writeout_term(&d->writeOut);
	vk_ssaa_resolve_term(&d->ssaaResolve);
	vk_quad_pipeline_term(&d->msQuadPipeline);
	vk_gpu_timer_term(&d->timer);
	vk_motion_pass_term(&d->motionPass, NULL);
	/* (the pipeline thread finishes what it is making for it now, while
	 * the shaders those pipelines are made of are still there) */
	vk_pipeline_mgr_term(&d->pipelines);
	vk_pipeline_mgr_construct(&d->pipelines, 1);
}

void vk_screen_drawer_free(vk_screen_drawer *d)
{
	vk_screen_drawer_term(d);
	screen_images_drop(d, 0);
	free(d->framebuffers);
	free(d->colorAttachments);
	free(d->hdrAttachments);
	free(d->depthAttachments);
	d->framebuffers = NULL;
	d->colorAttachments = d->hdrAttachments = d->depthAttachments = NULL;
	vk_attachment_free(&d->carriedPicture);
	vk_attachment_free(&d->depthAttachment);
	vk_attachment_free(&d->samplesAttachment);
	vk_quad_drawer_term(&d->lastPicture);
	if (d->hdrPipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(vkc.device, d->hdrPipeline, NULL);
	d->hdrPipeline = VK_NULL_HANDLE;
	if (d->renderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(vkc.device, d->renderPass, NULL);
	d->renderPass = VK_NULL_HANDLE;
	vk_tri_drawer_free(&d->tri);
	vk_base_drawer_term(&d->base);
}

static VkRenderPass make_screen_render_pass(bool upscaled, bool hdr)
{
	VkAttachmentDescription attachments[3];
	VkAttachmentReference colorReference, depthReference, pictureInput, hdrReference;
	VkSubpassDescription subpasses[2];
	VkSubpassDependency deps[8];
	VkSubpassDependency *d = deps;
	VkRenderPassCreateInfo info;
	VkRenderPass renderPass = VK_NULL_HANDLE;

	/* the picture */
	attachment_description(&attachments[0], vkc.colorFormat, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	/* the depth (kept for the upscaler, when there is one) */
	attachment_description(&attachments[1], vkc.depthFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
			upscaled ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	/* the HDR output's image */
	attachment_description(&attachments[2], vkc.hdrFormat, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	colorReference.attachment = 0;
	colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depthReference.attachment = 1;
	depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	pictureInput.attachment = 0;
	pictureInput.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	hdrReference.attachment = 2;
	hdrReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	memset(subpasses, 0, sizeof(subpasses));
	subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpasses[0].colorAttachmentCount = 1;
	subpasses[0].pColorAttachments = &colorReference;
	subpasses[0].pDepthStencilAttachment = &depthReference;
	/* the HDR output: the picture encoded where it is */
	subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpasses[1].inputAttachmentCount = 1;
	subpasses[1].pInputAttachments = &pictureInput;
	subpasses[1].colorAttachmentCount = 1;
	subpasses[1].pColorAttachments = &hdrReference;

	/* (the frontend reads the picture anywhere, from a render pass of its
	 * own or a copy: not by region, or it reads tiles not drawn yet) */
	d = dep(d, 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0);
	/* The depth attachment is the same one every frame: the frame before
	 * has to be done with it before this one clears it. Nothing said so.
	 * (The per-pixel renderer's render pass has the same, from upstream.)
	 * (The colour attachment is in it as well: once one dependency into the
	 * subpass is spelled out, the one Vulkan would have assumed for the
	 * rest is gone.) */
	d = dep(d, VK_SUBPASS_EXTERNAL, 0,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	/* The picture is cleared and drawn into an image the frontend was
	 * handed before, on the queue the two share: whatever read it there -
	 * a shader, a compute upscaler, a copy - is done before this clears it,
	 * or a GPU that draws in tiles shows tiles of the next frame with only
	 * its opaque polygons drawn yet. */
	d = dep(d, VK_SUBPASS_EXTERNAL, 0,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	/* the depth the frame leaves, for the motion vector pass and the upscaler */
	if (upscaled)
		d = dep(d, 0, VK_SUBPASS_EXTERNAL,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
					| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, 0);
	if (hdr)
	{
		/* the picture drawn, then encoded where it is... */
		d = dep(d, 0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT);
		/* ...into the image the frontend was done with... */
		d = dep(d, VK_SUBPASS_EXTERNAL, 1, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
		/* ...and which it is handed to read */
		d = dep(d, 1, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0);
	}

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = hdr ? 3 : 2;
	info.pAttachments = attachments;
	info.subpassCount = hdr ? 2 : 1;
	info.pSubpasses = subpasses;
	info.dependencyCount = (u32)(d - deps);
	info.pDependencies = deps;
	if (vkCreateRenderPass(vkc.device, &info, NULL, &renderPass) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return renderPass;
}

/* room for @count images' things */
static bool screen_images_room(vk_screen_drawer *d, u32 count)
{
	VkFramebuffer *f = (VkFramebuffer *)realloc(d->framebuffers, count * sizeof(VkFramebuffer));
	vk_attachment *c, *h, *z;
	if (f == NULL)
		return false;
	d->framebuffers = f;
	c = (vk_attachment *)realloc(d->colorAttachments, count * sizeof(vk_attachment));
	if (c == NULL)
		return false;
	d->colorAttachments = c;
	h = (vk_attachment *)realloc(d->hdrAttachments, count * sizeof(vk_attachment));
	if (h == NULL)
		return false;
	d->hdrAttachments = h;
	z = (vk_attachment *)realloc(d->depthAttachments, count * sizeof(vk_attachment));
	if (z == NULL)
		return false;
	d->depthAttachments = z;
	return true;
}

bool vk_screen_drawer_init(vk_screen_drawer *d, vk_samplers *samplers, vk_raster_modules *modules, u32 samples)
{
	vk_cmd_pool *pool = d->base.pool;
	const VkSampleCountFlagBits n = (VkSampleCountFlagBits)samples;
	/* (the upscaler wants the depth kept, which SSAA's samples are not) */
	const bool upscaled = upscaler_active() && samples == 1;
	const bool remade = d->viewport.width != vkc.width || d->viewport.height != vkc.height || samples != d->samples
			|| d->storagePictures != vk_writeout_wanted() || upscaled != d->upscaled;
	const bool hdr = vkc.hdr != NULL;
	const u32 size = (u32)vkc.swapChainSize;
	u32 i;
	d->modules = modules;
	/* (a change of SSAA is one of size: everything made for the size is
	 * made again; so is one of the write-out, whose pictures are made for
	 * it to write) */
	if (remade)
	{
		/* Not destroyed on the spot: the last picture was handed to the
		 * frontend, which shows it again for as long as no new one comes -
		 * and may, before the first frame at the new size is drawn. They
		 * go when the frames that could still use them have gone by.
		 * (Upstream: a crash starting the 240p test suite.) */
		if (pool != NULL)
		{
			const u32 image = (u32)d->tri.imageIndex;
			/* The image with the last frame does not go with the rest. A
			 * game that draws part of the screen has the rest from the
			 * last picture, and this is it: the next render starts from it
			 * as it does from one kept across a new context, at whatever
			 * size the screen now is - and here nothing is read back or
			 * loaded again. It goes after the next frame. */
			defer_attachment(pool, &d->carriedPicture);
			if (d->havePicture && image < d->images && d->colorAttachments[image].image != VK_NULL_HANDLE)
			{
				d->carriedPicture = d->colorAttachments[image];
				memset(&d->colorAttachments[image], 0, sizeof(vk_attachment));
				d->restoredPicture = d->carriedPicture.view;
				d->restoredSize = d->carriedPicture.extent;
			}
			for (i = 0; i < d->images; i++)
			{
				defer_framebuffer(pool, &d->framebuffers[i]);
				defer_attachment(pool, &d->colorAttachments[i]);
				defer_attachment(pool, &d->hdrAttachments[i]);
				defer_attachment(pool, &d->depthAttachments[i]);
			}
			defer_attachment(pool, &d->depthAttachment);
			defer_attachment(pool, &d->samplesAttachment);
			vk_motion_pass_term(&d->motionPass, pool);
		}
		screen_images_drop(d, 0);
		vk_attachment_free(&d->depthAttachment);
		vk_attachment_free(&d->samplesAttachment);
		memset(&d->depthAttachment, 0, sizeof(vk_attachment));
		memset(&d->samplesAttachment, 0, sizeof(vk_attachment));
		d->havePicture = false;
		if ((samples != d->samples || upscaled != d->upscaled) && d->renderPass != VK_NULL_HANDLE)
		{
			/* (the frames on the GPU were waited for: nothing uses it) */
			vkDestroyRenderPass(vkc.device, d->renderPass, NULL);
			d->renderPass = VK_NULL_HANDLE;
		}
		d->samples = samples;
		d->upscaled = upscaled;
		d->storagePictures = vk_writeout_wanted();
		upscaler_reset();
	}
	d->viewport.width = vkc.width;
	d->viewport.height = vkc.height;
	if (d->depthAttachment.image == VK_NULL_HANDLE && !upscaled)
	{
		/* (without SSAA, renders to textures draw into it too: as large as
		 * they need, vk_rtt_share_extent()) */
		const VkExtent2D depthSize = samples == 1 ? vk_rtt_share_extent(d->viewport) : d->viewport;
		if (!vk_attachment_init(&d->depthAttachment, depthSize.width, depthSize.height, vkc.depthFormat,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, true, n))
			return false;
	}
	/* SSAA: the samples' colour, which the last subpass resolves into the picture */
	if (samples > 1 && d->samplesAttachment.image == VK_NULL_HANDLE)
		if (!vk_attachment_init(&d->samplesAttachment, d->viewport.width, d->viewport.height, vkc.colorFormat,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
				false, n))
			return false;

	/* The HDR output: a last subpass encodes the picture, its input, into
	 * the HDR image, the frontend's (hdr_encode_vk.h) */
	if (d->renderPass == VK_NULL_HANDLE)
	{
		if (d->hdrPipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(vkc.device, d->hdrPipeline, NULL);
		d->hdrPipeline = VK_NULL_HANDLE;
		d->renderPass = samples > 1
				? vk_make_ssaa_render_pass(vkc.colorFormat, samples, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						hdr ? vkc.hdrFormat : VK_FORMAT_UNDEFINED)
				: make_screen_render_pass(upscaled, hdr);
		if (d->renderPass == VK_NULL_HANDLE)
			return false;
	}
	if (hdr && d->hdrPipeline == VK_NULL_HANDLE)
		d->hdrPipeline = vkc_hdr_subpass_pipeline(d->renderPass, samples > 1 ? 2 : 1, samples);

	if (d->images > size)
		screen_images_drop(d, size);
	else if (d->images < size)
	{
		/* (with SSAA: the picture, then the samples' colour and depth; then the HDR output's image) */
		VkImageView attachments[4];
		const u32 count = (samples > 1 ? 3 : 2) + (hdr ? 1 : 0);
		if (!screen_images_room(d, size))
			return false;
		attachments[0] = VK_NULL_HANDLE;
		attachments[1] = samples > 1 ? d->samplesAttachment.view : upscaled ? VK_NULL_HANDLE : d->depthAttachment.view;
		attachments[2] = samples > 1 ? d->depthAttachment.view : VK_NULL_HANDLE;
		attachments[3] = VK_NULL_HANDLE;
		for (; d->images < size; d->images++)
		{
			const u32 at = d->images;
			VkFramebufferCreateInfo createInfo;
			d->framebuffers[at] = VK_NULL_HANDLE;
			memset(&d->colorAttachments[at], 0, sizeof(vk_attachment));
			memset(&d->hdrAttachments[at], 0, sizeof(vk_attachment));
			memset(&d->depthAttachments[at], 0, sizeof(vk_attachment));
			if (upscaled)
			{
				/* the frame's depth, kept, sampled and copied from (the frontend reads it as it likes) */
				if (!vk_attachment_init(&d->depthAttachments[at], d->viewport.width, d->viewport.height, vkc.depthFormat,
						VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
						false, VK_SAMPLE_COUNT_1_BIT))
					return false;
				attachments[1] = d->depthAttachments[at].view;
			}
			/* (copied from when the context goes, to keep the picture: rend/last_picture.h; the HDR output's input) */
			if (!vk_attachment_init(&d->colorAttachments[at], d->viewport.width, d->viewport.height, vkc.colorFormat,
					VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
					| (d->storagePictures ? VK_IMAGE_USAGE_STORAGE_BIT : 0)
					| (hdr ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0), false, VK_SAMPLE_COUNT_1_BIT))
				return false;
			attachments[0] = d->colorAttachments[at].view;
			if (hdr)
			{
				if (!vk_attachment_init(&d->hdrAttachments[at], d->viewport.width, d->viewport.height, vkc.hdrFormat,
						VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
						false, VK_SAMPLE_COUNT_1_BIT))
					return false;
				attachments[count - 1] = d->hdrAttachments[at].view;
			}
			memset(&createInfo, 0, sizeof(createInfo));
			createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			createInfo.renderPass = d->renderPass;
			createInfo.attachmentCount = count;
			createInfo.pAttachments = attachments;
			createInfo.width = d->viewport.width;
			createInfo.height = d->viewport.height;
			createInfo.layers = 1;
			if (vkCreateFramebuffer(vkc.device, &createInfo, NULL, &d->framebuffers[at]) != VK_SUCCESS)
			{
				d->images++;	/* (its attachments go with the rest) */
				return false;
			}
		}
	}

	if (!vk_pipeline_mgr_init(&d->pipelines, modules, d->renderPass, samples))
		return false;
	if (!vk_tri_drawer_init(&d->tri, samplers, &d->pipelines))
		return false;
	if (samples > 1)
	{
		if (!vk_ssaa_resolve_init(&d->ssaaResolve, d->renderPass, 1, samples, d->samplesAttachment.view)
				|| !vk_quad_pipeline_init(&d->msQuadPipeline, modules, d->renderPass, 0, samples))
			return false;
	}
	if (upscaled)
	{
		VkImageView *depthViews = (VkImageView *)malloc(d->images * sizeof(VkImageView));
		bool made;
		if (depthViews == NULL)
			return false;
		for (i = 0; i < d->images; i++)
			depthViews[i] = d->depthAttachments[i].view;
		made = vk_motion_pass_init(&d->motionPass, d->viewport, vkc.depthFormat, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				depthViews, d->images, d->pipelines.pipelineLayout, modules->m[RSPV_MV_VERT], modules->m[RSPV_MV_FRAG], pool);
		free(depthViews);
		if (!made)
			return false;
	}
	vk_gpu_timer_init(&d->timer, vkc.swapChainSize, "per-triangle renderer");
	if (remade)
	{
		VkImageView *views = NULL;
		bool made;
		if (d->storagePictures)
		{
			views = (VkImageView *)malloc(d->images * sizeof(VkImageView));
			if (views == NULL)
				return false;
			for (i = 0; i < d->images; i++)
				views[i] = d->colorAttachments[i].view;
		}
		made = vk_writeout_init(&d->writeOut, views, d->storagePictures ? d->images : 0, d->viewport.width, d->viewport.height);
		free(views);
		if (!made)
			return false;
	}
	return true;
}

bool vk_screen_drawer_set_quad_pipeline(vk_screen_drawer *d, vk_quad_pipeline *pipeline)
{
	d->quadPipeline = pipeline;
	/* (with SSAA the picture goes into the samples: a pipeline of its own for them) */
	return vk_quad_drawer_init(&d->lastPicture, d->samples > 1 ? &d->msQuadPipeline : pipeline);
}

static bool screen_begin(vk_screen_drawer *d)
{
	vk_base_drawer *b = &d->base;
	vk_cmd_pool *pool = b->pool;
	VkCommandBufferBeginInfo begin;
	VkClearValue clear[3];
	VkRenderPassBeginInfo info;
	VkViewport viewport;
	VkCommandBuffer cmd;
	int image;

	vk_tri_drawer_new_image(&d->tri);
	image = d->tri.imageIndex;
	cmd = vk_cmd_pool_allocate(pool);
	if (cmd == VK_NULL_HANDLE)
		return false;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	transform_matrix_calc(&b->matrices, &pvrrc);
	/* the frame drawn under this frame's jitter, its strips matched for the motion vector pass */
	if (d->upscaled && vk_motion_pass_begin(&d->motionPass, image))
	{
		float jx, jy;
		upscaler_jitter(&jx, &jy);
		transform_matrix_jitter(&b->matrices, jx, jy);
	}
	b->vertexUniformBuffer = vk_base_drawer_write_vertex_uniforms(b, cmd, image);
	if (b->vertexUniformBuffer == VK_NULL_HANDLE)
		return false;
	/* (the HDR output's transform, for the render pass's last subpass) */
	if (d->hdrPipeline != VK_NULL_HANDLE)
		vkc_hdr_prepare(pool, (unsigned)pool->index, (unsigned)pool->frames, cmd);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to; any other
	 * starts black, and has the last picture put into it below. */
	memset(clear, 0, sizeof(clear));
	if (pvrrc.clearFramebuffer)
	{
		clear[0].color.float32[0] = VO_BORDER_COL.Red / 255.f;
		clear[0].color.float32[1] = VO_BORDER_COL.Green / 255.f;
		clear[0].color.float32[2] = VO_BORDER_COL.Blue / 255.f;
	}
	clear[0].color.float32[3] = 1.f;
	vk_gpu_timer_begin(&d->timer, cmd, pool->index);
	/* (with SSAA: the picture, all of which the resolve writes, then the samples' colour and depth) */
	if (d->samples > 1)
		clear[1] = clear[0];
	clear[d->samples > 1 ? 2 : 1].depthStencil.depth = 0.f;
	clear[d->samples > 1 ? 2 : 1].depthStencil.stencil = 0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = d->renderPass;
	info.framebuffer = d->framebuffers[image];
	info.renderArea.extent = d->viewport;
	info.clearValueCount = d->samples > 1 ? 3 : 2;
	info.pClearValues = clear;
	vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	viewport.x = viewport.y = 0.f;
	viewport.width = (float)d->viewport.width;
	viewport.height = (float)d->viewport.height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	vk_base_drawer_set_base_scissor(b);
	/* A render that covers only part of the screen leaves the rest of the
	 * framebuffer as it was. Every frame here goes to a fresh image,
	 * cleared, so the part that is not drawn was black: the last frame is
	 * drawn into it first, whole, and the render goes over it. Only for
	 * such a render - one that covers the screen pays nothing - and not
	 * into a framebuffer the game has not drawn to lately, which starts
	 * empty. */
	if ((d->havePicture || d->restoredPicture != VK_NULL_HANDLE) && d->quadPipeline != NULL && !pvrrc.clearFramebuffer
			&& transform_matrix_clipped(&b->matrices))
	{
		static const float opaque[4] = { 1.f, 1.f, 1.f, 1.f };
		const int count = (int)d->images;
		VkRect2D whole;
		VkPipeline pipeline;
		whole.offset.x = whole.offset.y = 0;
		whole.extent = d->viewport;
		vkCmdSetScissor(cmd, 0, 1, &whole);
		vkCmdSetBlendConstants(cmd, opaque);
		pipeline = vk_quad_pipeline_get(d->samples > 1 ? &d->msQuadPipeline : d->quadPipeline, false);
		if (pipeline == VK_NULL_HANDLE)
			return false;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		/* (the picture kept from another context can be of another size) */
		if (d->havePicture)
			vk_quad_drawer_draw(&d->lastPicture, cmd, d->colorAttachments[(image + count - 1) % count].view, NULL, true, pool);
		else
		{
			QuadVertex area[4];
			vk_quad_drawer_draw(&d->lastPicture, cmd, d->restoredPicture,
					vk_quad_game_area(area, d->restoredSize.width, d->restoredSize.height, d->viewport.width, d->viewport.height)
						? area : NULL, false, pool);
		}
	}
	vkCmdSetScissor(cmd, 0, 1, &b->baseScissor);
	d->cmd = cmd;
	return true;
}

bool vk_screen_drawer_draw(vk_screen_drawer *d, VkImageView fogView, VkImageView paletteView)
{
	memset(&d->base.currentScissor, 0, sizeof(d->base.currentScissor));
	/* (which works the matrices out for this render, and writes the vertex
	 * uniforms from them) */
	return screen_begin(d) && vk_tri_drawer_draw(&d->tri, d->cmd, fogView, paletteView, d->samples, &d->ssaaResolve,
			d->viewport.width, d->viewport.height);
}

void vk_screen_drawer_keep_picture(vk_screen_drawer *d)
{
	u8 *pixels;
	if (!d->havePicture)
		return;
	pixels = last_picture_keep((int)d->viewport.width, (int)d->viewport.height);
	if (pixels != NULL)
		vk_read_picture(d->colorAttachments[d->tri.imageIndex].image, vkc.colorFormat, d->viewport.width, d->viewport.height, pixels);
}

bool vk_screen_drawer_writeout_before_osd(vk_screen_drawer *d)
{
	const u32 image = (u32)d->tri.imageIndex;
	if (!vk_writeout_runs(&d->writeOut, d->viewport.height))
		return false;
	/* (the HDR output's subpass passed over: the picture is encoded once it is written out) */
	if (d->hdrPipeline != VK_NULL_HANDLE)
		vkCmdNextSubpass(d->cmd, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(d->cmd);
	vk_writeout_record(&d->writeOut, d->cmd, image, d->colorAttachments[image].image, d->colorAttachments[image].view,
			d->viewport.width, d->viewport.height);
	vk_writeout_begin_overlay(&d->writeOut, d->cmd, image);
	d->overlaid = true;
	return true;
}

bool vk_screen_drawer_end(vk_screen_drawer *d)
{
	vk_cmd_pool *pool = d->base.pool;
	const u32 image = (u32)d->tri.imageIndex;
	VkCommandBuffer cmd = d->cmd;
	VkImage picture = d->colorAttachments[image].image;
	VkImageView view = d->colorAttachments[image].view;
	bool hdrAfter = false;
	bool hdr = false;
	bool ok = true;
	d->havePicture = true;
	d->restoredPicture = VK_NULL_HANDLE;
	/* (this frame may have drawn it: it goes when the frame is done with) */
	defer_attachment(pool, &d->carriedPicture);
	/* The HDR output: the picture encoded in the render pass's last
	 * subpass, where it is; on a frame the chip's write-out changes after
	 * the render pass, once that is done (and what goes over it drawn), in
	 * the same submission */
	if (d->hdrPipeline != VK_NULL_HANDLE && !d->overlaid)
	{
		vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
		/* (or with the post effects, which only the pass of its own draws) */
		hdrAfter = vk_writeout_runs(&d->writeOut, d->viewport.height) || vkc_hdr_post_active();
		if (!hdrAfter)
			vkc_hdr_subpass_draw(pool, (unsigned)pool->index, cmd, d->hdrPipeline, view,
					d->samples > 1 ? d->samplesAttachment.view : VK_NULL_HANDLE, d->viewport.width, d->viewport.height);
	}
	else if (d->hdrPipeline != VK_NULL_HANDLE)
		hdrAfter = true;
	vkCmdEndRenderPass(cmd);
	if (d->overlaid)
		d->overlaid = false;
	else
		vk_writeout_record(&d->writeOut, cmd, image, picture, view, d->viewport.width, d->viewport.height);
	if (d->motionPass.recorded)
	{
		/* (what went over the picture may have bound its own) */
		vk_desc_sets_bind_per_frame(vk_tri_drawer_desc_set(&d->tri), cmd);
		ok = vk_motion_pass_record(&d->motionPass, cmd, (int)image, d->base.geometry, d->tri.offsets.indexOffset, &d->base.baseScissor,
				d->base.geometryVertices);
	}
	vk_gpu_timer_end(&d->timer, cmd, pool->index);
	vkEndCommandBuffer(cmd);
	d->cmd = VK_NULL_HANDLE;
	if (hdrAfter && vkc.hdr != NULL)
	{
		/* (in a command buffer of its own, after the frame's) */
		VkCommandBuffer encode = vk_cmd_pool_allocate(pool);
		VkCommandBufferBeginInfo begin;
		VkImage i = picture;
		VkImageView v = view;
		if (encode == VK_NULL_HANDLE)
			return false;
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(encode, &begin);
		hdr = vkc_hdr_encode(pool, (unsigned)pool->index, (unsigned)pool->frames, encode, &i, &v, d->viewport.width, d->viewport.height,
				VK_NULL_HANDLE);
		vkEndCommandBuffer(encode);
		if (hdr)
		{
			picture = i;
			view = v;
		}
	}
	else if (d->hdrPipeline != VK_NULL_HANDLE)
	{
		picture = d->hdrAttachments[image].image;
		view = d->hdrAttachments[image].view;
		hdr = true;
	}
	vk_cmd_pool_end_frame(pool);
	vkc_present_frame(picture, view, (s32)d->viewport.width, (s32)d->viewport.height, hdr);
	if (d->motionPass.recorded)
		vk_motion_pass_submit(&d->motionPass, (int)image, d->depthAttachments[image].image, d->depthAttachments[image].view,
				vkc.depthFormat);
	return ok;
}

/* ---- A render to a texture's ---- */

void vk_rtt_drawer_construct(vk_rtt_drawer *d)
{
	memset(d, 0, sizeof(*d));
	vk_base_drawer_construct(&d->base);
	vk_tri_drawer_construct(&d->tri, &d->base);
	vk_pipeline_mgr_construct(&d->pipelines, 2);
	vk_ssaa_resolve_construct(&d->ssaaResolve);
	d->samples = 1;
}

/* a framebuffer's place for each image (made again only for other
 * attachments: a texture rendered to every frame is the same one) */
static bool rtt_framebuffers(vk_rtt_drawer *d, u32 count)
{
	vk_kept_framebuffer *more;
	while (d->framebufferCount > count)
		vk_kept_framebuffer_free(&d->framebuffers[--d->framebufferCount]);
	if (d->framebufferCount == count)
		return true;
	more = (vk_kept_framebuffer *)realloc(d->framebuffers, count * sizeof(vk_kept_framebuffer));
	if (more == NULL)
		return false;
	d->framebuffers = more;
	memset(&more[d->framebufferCount], 0, (count - d->framebufferCount) * sizeof(vk_kept_framebuffer));
	d->framebufferCount = count;
	return true;
}

void vk_rtt_drawer_term(vk_rtt_drawer *d)
{
	if (d->drewShared + d->drewOwn != 0)
		NOTICE_LOG(RENDERER, "Renders to textures: %u drawn with the screen's depth, %u with one of their own; %u cleared outside\n",
				d->drewShared, d->drewOwn, vk_texture_outside_cleared);
	vk_texture_outside_cleared = 0;
	d->drewShared = d->drewOwn = 0;
	vk_ssaa_resolve_term(&d->ssaaResolve);
	/* (as the screen's drawer's) */
	vk_pipeline_mgr_term(&d->pipelines);
	vk_pipeline_mgr_construct(&d->pipelines, 2);
}

void vk_rtt_drawer_free(vk_rtt_drawer *d)
{
	vk_rtt_drawer_term(d);
	rtt_framebuffers(d, 0);
	free(d->framebuffers);
	d->framebuffers = NULL;
	vk_attachment_free(&d->depthAttachment);
	vk_attachment_free(&d->samplesAttachment);
	vk_tri_drawer_free(&d->tri);
	vk_base_drawer_term(&d->base);
}

static bool rtt_ssaa_attachments(vk_rtt_drawer *d, u32 side)
{
	const VkSampleCountFlagBits n = (VkSampleCountFlagBits)d->samples;
	defer_attachment(d->base.pool, &d->depthAttachment);
	defer_attachment(d->base.pool, &d->samplesAttachment);
	if (!vk_attachment_init(&d->depthAttachment, side, side, vkc.depthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, true, n)
			|| !vk_attachment_init(&d->samplesAttachment, side, side, VK_FORMAT_R8G8B8A8_UNORM,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
				false, n))
		return false;
	if (d->ssaaResolve.descSet != VK_NULL_HANDLE && d->base.pool != NULL)
		vk_ssaa_resolve_set_samples_view(&d->ssaaResolve, d->samplesAttachment.view, d->base.pool);
	return true;
}

bool vk_rtt_drawer_init(vk_rtt_drawer *d, vk_samplers *samplers, vk_raster_modules *modules, vk_texcache *textureCache, u32 samples)
{
	if (!vk_pipeline_mgr_rtt_init(&d->pipelines, modules, samples) || !vk_tri_drawer_init(&d->tri, samplers, &d->pipelines))
		return false;
	d->textureCache = textureCache;
	if (samples != d->samples)
	{
		/* (the depth attachment is of the other kind: one of the right kind is made below or when first needed) */
		defer_attachment(d->base.pool, &d->depthAttachment);
		defer_attachment(d->base.pool, &d->samplesAttachment);
		d->samples = samples;
	}
	/* The attachments are made now, never while the game runs: SSAA's, as
	 * large as a render to a texture can be (where SSAA is done to them,
	 * memory made only if it is needed backs them); the depth is the
	 * screen's, or one of its own (vk_rtt_drawer_share_depth()). And a
	 * framebuffer's place for each image. */
	if (samples > 1)
	{
		if ((d->samplesAttachment.image == VK_NULL_HANDLE || d->samplesAttachment.extent.width < vk_rtt_max_side())
				&& !rtt_ssaa_attachments(d, vk_rtt_max_side()))
			return false;
		if (!vk_ssaa_resolve_init(&d->ssaaResolve, d->pipelines.renderPass, 1, samples, d->samplesAttachment.view))
			return false;
	}
	else
		vk_ssaa_resolve_term(&d->ssaaResolve);
	return rtt_framebuffers(d, (u32)vkc.swapChainSize);
}

bool vk_rtt_drawer_share_depth(vk_rtt_drawer *d, const vk_screen_drawer *screen)
{
	const vk_attachment *shared;
	VkExtent2D common;
	d->screen = screen;
	if (d->samples > 1)
		return true;
	shared = screen != NULL ? vk_screen_drawer_shared_depth(screen) : NULL;
	if (shared != NULL)
	{
		/* (its own goes once the frames that may use it are done) */
		defer_attachment(d->base.pool, &d->depthAttachment);
		return true;
	}
	common = vk_rtt_common_render();
	if (d->depthAttachment.image == VK_NULL_HANDLE || d->depthAttachment.extent.width < common.width
			|| d->depthAttachment.extent.height < common.height)
	{
		defer_attachment(d->base.pool, &d->depthAttachment);
		return vk_attachment_init(&d->depthAttachment, common.width, common.height, vkc.depthFormat,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, true, VK_SAMPLE_COUNT_1_BIT);
	}
	return true;
}

static bool rtt_begin(vk_rtt_drawer *d)
{
	vk_base_drawer *b = &d->base;
	vk_cmd_pool *pool = b->pool;
	/* the frame from its first pixel, as far as the pixel clip goes (see transform_matrix.h) */
	const u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	const u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 clipX = pvrrc.fb_X_CLIP.min, clipY = pvrrc.fb_Y_CLIP.min;
	const u32 clipWidth = origWidth - clipX, clipHeight = origHeight - clipY;
	const u32 scale = vk_rtt_scale();
	u32 upscaledWidth = origWidth;
	u32 upscaledHeight = origHeight;
	u32 heightPow2 = 8;
	u32 widthPow2 = 8;
	u32 areaWidth, areaHeight;
	u32 tcw, tsp, u, v;
	const vk_attachment *depth = NULL;
	vk_texture *texture;
	VkImageLayout layout;
	VkImageView views[3];
	u64 serials[3];
	VkFramebuffer framebuffer;
	VkClearValue clear[3];
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo info;
	VkViewport viewport;
	VkCommandBuffer cmd;
	const VkImageUsageFlags renderTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT;	/* read back when the game touches it in video memory */
	int image;

	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	transform_matrix_calc(&b->matrices, &pvrrc);

	d->textureAddr = FB_W_SOF1 & VRAM_MASK;
	while (heightPow2 < upscaledHeight)
		heightPow2 *= 2;
	while (widthPow2 < upscaledWidth)
		widthPow2 *= 2;
	if (scale > 1)
	{
		upscaledWidth *= scale;
		upscaledHeight *= scale;
		widthPow2 *= scale;
		heightPow2 *= scale;
	}

	vk_tri_drawer_new_image(&d->tri);
	image = d->tri.imageIndex;
	cmd = vk_cmd_pool_allocate(pool);
	if (cmd == VK_NULL_HANDLE)
		return false;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	d->cmd = cmd;
	b->vertexUniformBuffer = vk_base_drawer_write_vertex_uniforms(b, cmd, image);
	if (b->vertexUniformBuffer == VK_NULL_HANDLE)
		return false;

	if (d->samples > 1)
	{
		/* (SSAA's attachments were made as large as a texture rendered to
		 * can be; a pixel clip beyond the texture's largest size makes one
		 * larger, which is the only time they are made while a game runs) */
		const u32 side = widthPow2 > heightPow2 ? widthPow2 : heightPow2;
		if (side > d->samplesAttachment.extent.width)
		{
			WARN_LOG(RENDERER, "Render to a texture of %u x %u: SSAA's attachments made larger\n", widthPow2, heightPow2);
			if (!rtt_ssaa_attachments(d, side))
				return false;
		}
	}
	/* What the render draws: its own pixels, the texture's from the first;
	 * with SSAA all of the texture, its attachments being that large */
	areaWidth = d->samples > 1 ? widthPow2 : upscaledWidth;
	areaHeight = d->samples > 1 ? heightPow2 : upscaledHeight;
	/* The depth: the screen's when it is large enough - drawn into by the
	 * screen's frames and this one by one, on the same queue, each
	 * clearing it first: the render passes' dependencies on the depth
	 * before them are those of one frame after another -, or this
	 * drawer's own; which a render larger than both has made for it. */
	if (d->samples > 1)
		depth = &d->depthAttachment;
	else
	{
		const vk_attachment *shared = d->screen != NULL ? vk_screen_drawer_shared_depth(d->screen) : NULL;
		if (shared != NULL && areaWidth <= shared->extent.width && areaHeight <= shared->extent.height)
		{
			depth = shared;
			d->drewShared++;
		}
		else
		{
			d->drewOwn++;
			if (d->depthAttachment.image == VK_NULL_HANDLE || areaWidth > d->depthAttachment.extent.width
					|| areaHeight > d->depthAttachment.extent.height)
			{
				const bool had = d->depthAttachment.image != VK_NULL_HANDLE;
				const u32 w = had && d->depthAttachment.extent.width > areaWidth ? d->depthAttachment.extent.width : areaWidth;
				const u32 h = had && d->depthAttachment.extent.height > areaHeight ? d->depthAttachment.extent.height : areaHeight;
				if (!d->saidLarger)
				{
					d->saidLarger = true;
					WARN_LOG(RENDERER, "Render to a texture of %u x %u: a depth made for it\n", areaWidth, areaHeight);
				}
				defer_attachment(pool, &d->depthAttachment);
				if (!vk_attachment_init(&d->depthAttachment, w, h, vkc.depthFormat,
						VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, true, VK_SAMPLE_COUNT_1_BIT))
					return false;
			}
			depth = &d->depthAttachment;
		}
	}

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_read.h */
	vk_rtt_supersede(d->textureAddr, clipX, clipY, clipWidth, clipHeight);
	/* TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1 */
	tcw = TAW_SET(TAW_SET(0, TCW_TexAddr, d->textureAddr >> 3), TCW_ScanOrder, 1);
	switch (FB_W_CTRL.fb_packmode)
	{
	case 0:
	case 3:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_1555);
		break;
	case 1:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_565);
		break;
	case 2:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_4444);
		break;
	}
	for (u = 0; u < 7 && (8u << u) < origWidth; u++)
		;
	for (v = 0; v < 7 && (8u << v) < origHeight; v++)
		;
	tsp = TAW_SET(TAW_SET(0, TSP_TexU, u), TSP_TexV, v);

	texture = vk_texcache_get(d->textureCache, tsp, tcw, 0);
	if (vk_texture_is_new(texture))
		texcache_create(&texture->tc);
	else if (vk_texcache_is_in_flight(d->textureCache, texture))
	{
		vk_texture_keep_read_only_view(texture);
		vk_texcache_destroy_later(d->textureCache, texture);
	}
	vk_texcache_set_in_flight(d->textureCache, texture);
	d->texture = texture;

	/* A texture the game uploaded can become one it renders to, at the
	 * same address, the same size and in this format: its image was not
	 * made to be rendered to, nor to be copied from, and is made again. */
	if (texture->format != VK_FORMAT_R8G8B8A8_UNORM || texture->extent.width != widthPow2 || texture->extent.height != heightPow2
			|| (texture->imageUsage & renderTargetUsage) != renderTargetUsage)
	{
		texture->extent.width = widthPow2;
		texture->extent.height = heightPow2;
		texture->format = VK_FORMAT_R8G8B8A8_UNORM;
		texture->needsStaging = true;
		if (!vk_texture_create_render_target(texture, renderTargetUsage))
			return false;
		layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	else
		layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	d->width = areaWidth;
	d->height = areaHeight;
	if (d->samples == 1)
		vk_texture_clear_outside(texture, cmd, areaWidth, areaHeight, &layout);

	vk_set_image_layout(cmd, vk_texture_image(texture), VK_FORMAT_R8G8B8A8_UNORM, 1, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	/* (with SSAA: the texture, then the samples' colour and depth) */
	views[0] = vk_texture_view(texture);
	views[1] = d->samples > 1 ? d->samplesAttachment.view : depth->view;
	views[2] = depth->view;
	serials[0] = vk_texture_view_serial(texture);
	serials[1] = d->samples > 1 ? d->samplesAttachment.viewSerial : depth->viewSerial;
	serials[2] = depth->viewSerial;
	if (!rtt_framebuffers(d, (u32)vkc.swapChainSize))
		return false;
	framebuffer = vk_kept_framebuffer_get(&d->framebuffers[image], d->pipelines.renderPass, d->samples > 1 ? 3 : 2,
			views, serials, d->width, d->height);
	if (framebuffer == VK_NULL_HANDLE)
		return false;

	memset(clear, 0, sizeof(clear));
	clear[0].color.float32[3] = 1.f;
	if (d->samples > 1)
		clear[1] = clear[0];
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = d->pipelines.renderPass;
	info.framebuffer = framebuffer;
	info.renderArea.extent.width = d->width;
	info.renderArea.extent.height = d->height;
	info.clearValueCount = d->samples > 1 ? 3 : 2;
	info.pClearValues = clear;
	vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	viewport.x = viewport.y = 0.f;
	viewport.width = (float)upscaledWidth;
	viewport.height = (float)upscaledHeight;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	/* only the pixel clip is written */
	b->baseScissor.offset.x = (s32)(clipX * scale);
	b->baseScissor.offset.y = (s32)(clipY * scale);
	b->baseScissor.extent.width = clipWidth * scale;
	b->baseScissor.extent.height = clipHeight * scale;
	vkCmdSetScissor(cmd, 0, 1, &b->baseScissor);
	return true;
}

bool vk_rtt_drawer_draw(vk_rtt_drawer *d, VkImageView fogView, VkImageView paletteView)
{
	memset(&d->base.currentScissor, 0, sizeof(d->base.currentScissor));
	return rtt_begin(d) && vk_tri_drawer_draw(&d->tri, d->cmd, fogView, paletteView, d->samples, &d->ssaaResolve, d->width, d->height);
}

void vk_rtt_drawer_end(vk_rtt_drawer *d)
{
	vk_texture *texture = d->texture;
	vkCmdEndRenderPass(d->cmd);
	vkEndCommandBuffer(d->cmd);
	d->cmd = VK_NULL_HANDLE;
	vk_cmd_pool_end_frame(d->base.pool);

	texture->tc.dirty = 0;
	texture->tc.stride_read = 0;	/* (rendered, not read) */
	texture->tc.made_type = 0;
	libCore_vramlock_Lock(texture->tc.sa_tex, texture->tc.sa + texture->tc.size - 1, &texture->tc);
	/* video memory gets it when something first touches it there */
	vk_rtt_watch_texture(texture, d->textureAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
}
