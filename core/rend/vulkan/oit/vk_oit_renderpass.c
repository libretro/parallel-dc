/*
    Created on: Nov 10, 2019

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
/* See vk_oit_renderpass.h */
#include <string.h>
#include <retro_miscellaneous.h>
#include "vk_oit_renderpass.h"

void vk_oit_passes_construct(vk_oit_passes *p, bool rtt)
{
	memset(p, 0, sizeof(*p));
	p->samples = 1;
	p->rtt = rtt;
}

static void attachment(VkAttachmentDescription *a, VkFormat format, VkSampleCountFlagBits samples,
		VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkAttachmentLoadOp stencilLoadOp,
		VkImageLayout initialLayout, VkImageLayout finalLayout)
{
	memset(a, 0, sizeof(*a));
	a->format = format;
	a->samples = samples;
	a->loadOp = loadOp;
	a->storeOp = storeOp;
	a->stencilLoadOp = stencilLoadOp;
	a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	a->initialLayout = initialLayout;
	a->finalLayout = finalLayout;
}

static void subpass(VkSubpassDescription *s, u32 inputCount, const VkAttachmentReference *inputs,
		const VkAttachmentReference *color, const VkAttachmentReference *depth)
{
	memset(s, 0, sizeof(*s));
	s->pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	s->inputAttachmentCount = inputCount;
	s->pInputAttachments = inputs;
	s->colorAttachmentCount = 1;
	s->pColorAttachments = color;
	s->pDepthStencilAttachment = depth;
}

/* one dependency, at @d; the next place */
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

static VkRenderPass create(const VkAttachmentDescription *attachments, u32 attachmentCount, const VkSubpassDescription *subpasses,
		u32 subpassCount, const VkSubpassDependency *deps, u32 depCount)
{
	VkRenderPassCreateInfo info;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = attachmentCount;
	info.pAttachments = attachments;
	info.subpassCount = subpassCount;
	info.pSubpasses = subpasses;
	info.dependencyCount = depCount;
	info.pDependencies = deps;
	if (vkCreateRenderPass(vkc.device, &info, NULL, &renderPass) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return renderPass;
}

/* SSAA: attachment 0 is the picture, of one sample; 1 and 2 the colour
 * and depth of the samples, one image each for all the render passes of a
 * frame. The first render pass clears them; one that is not the last
 * stores them for the next, the last leaves them: with one render pass
 * they are never stored, and a tiler never backs them. The picture is all
 * written by the last subpass of the last render pass, so it is never
 * loaded, and stored by the last only. The colour samples are read and
 * written in subpass 2 (each put together where it is) and read in 3:
 * they are in the general layout there. */
static VkRenderPass make_ssaa(const vk_oit_passes *p, VkFormat colorFormat, bool initial, bool last, bool depthWrites, bool resumed)
{
	const VkSampleCountFlagBits n = (VkSampleCountFlagBits)p->samples;
	const VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	const VkPipelineStageFlags fbStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	const VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_SHADER_WRITE_BIT;
	const VkAccessFlags all = writes | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
			| VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	VkAttachmentDescription attachments[3];
	VkAttachmentReference pictureReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference colorReference = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReference = { 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReadOnlyRef = { 2, VK_IMAGE_LAYOUT_UNDEFINED };
	VkAttachmentReference colorGeneral = { 1, VK_IMAGE_LAYOUT_GENERAL };
	VkSubpassDescription subpasses[4];
	VkSubpassDependency deps[14];
	VkSubpassDependency *d = deps;
	u32 s;

	depthReadOnlyRef.layout = vk_oit_depth_input_layout(depthWrites);
	attachment(&attachments[0], colorFormat, VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, last ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	attachment(&attachments[1], colorFormat, n,
			initial ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			last ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
	attachment(&attachments[2], vkc.depthFormat, n,
			initial ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			last ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_CLEAR, initial ? VK_IMAGE_LAYOUT_UNDEFINED : depthLayout, depthLayout);
	if (resumed)
	{
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].initialLayout = attachments[0].finalLayout;
	}
	/* depth and stencil, modifier volumes */
	subpass(&subpasses[0], 0, NULL, &colorReference, &depthReference);
	/* colour, and the translucent lists */
	subpass(&subpasses[1], 1, &depthReadOnlyRef, &colorReference, &depthReadOnlyRef);
	/* translucent modifier volumes; each sample put together and the translucent depth, before another render pass */
	subpass(&subpasses[2], 1, &colorGeneral, &colorGeneral, &depthReference);
	/* the picture put together and resolved */
	subpass(&subpasses[3], 1, &colorGeneral, &pictureReference, NULL);

	/* The samples, the lists and the picture's image are the same ones for
	 * every render pass and every frame: what came before is done with
	 * them before any subpass uses them */
	for (s = 0; s < 4; s++)
		d = dep(d, VK_SUBPASS_EXTERNAL, s, fbStages, fbStages, writes | VK_ACCESS_SHADER_READ_BIT, all, 0);
	/* each subpass reads and writes what the one before wrote, where it is */
	for (s = 0; s < 3; s++)
		d = dep(d, s, s + 1, fbStages, fbStages, writes, all, VK_DEPENDENCY_BY_REGION_BIT);
	/* (the barriers between the passes over the lists in subpasses 2 and 3) */
	for (s = 2; s < 4; s++)
		d = dep(d, s, s, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_DEPENDENCY_BY_REGION_BIT);
	/* what the render pass leaves - the picture, to be shown or sampled; the samples, for the next render pass */
	for (s = 2; s < 4; s++)
		d = dep(d, s, VK_SUBPASS_EXTERNAL, fbStages, fbStages | VK_PIPELINE_STAGE_TRANSFER_BIT,
				writes, all | VK_ACCESS_TRANSFER_READ_BIT, 0);

	return create(attachments, ARRAY_SIZE(attachments), subpasses, ARRAY_SIZE(subpasses), deps, (u32)(d - deps));
}

static VkRenderPass make(const vk_oit_passes *p, bool initial, bool last, bool depthWrites, bool resumed)
{
	const VkFormat colorFormat = p->rtt ? VK_FORMAT_R8G8B8A8_UNORM : vkc.colorFormat;
	/* The colour and depth attachments the polygons are drawn into, and the
	 * per-pixel lists, are the same ones for every render pass and every
	 * frame: the one before may still be reading or writing them when this
	 * one clears or loads them and writes the lists again. Nothing said it
	 * had to be done first. Most drivers work that out for themselves; the
	 * ones that do not show one frame's leavings in the next. (From
	 * upstream, which names MoltenVK on macOS 15.) */
	const VkPipelineStageFlags oitStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkAttachmentDescription attachments[3];
	VkAttachmentReference swapChainReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference colorReference = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReference = { 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReadOnlyRef = { 2, VK_IMAGE_LAYOUT_UNDEFINED };
	VkAttachmentReference colorInput = { 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkSubpassDescription subpasses[3];
	VkSubpassDependency deps[16];
	VkSubpassDependency *d = deps;
	u32 s;

	if (p->samples > 1)
		return make_ssaa(p, colorFormat, initial, last, depthWrites, resumed);
	depthReadOnlyRef.layout = vk_oit_depth_input_layout(depthWrites);
	/* the picture */
	attachment(&attachments[0], colorFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	/* opaque and punch-through colour */
	attachment(&attachments[1], colorFormat, VK_SAMPLE_COUNT_1_BIT,
			initial ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			last ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	/* opaque and punch-through depth (kept after the last render pass for the upscaler, when there is one) */
	attachment(&attachments[2], vkc.depthFormat, VK_SAMPLE_COUNT_1_BIT,
			initial ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			last && !p->keepDepth ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
	if (resumed)
	{
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].initialLayout = attachments[0].finalLayout;
	}
	/* depth and modifier volumes (it need not have the colour attachment) */
	subpass(&subpasses[0], 0, NULL, &colorReference, &depthReference);
	/* colour */
	subpass(&subpasses[1], 1, &depthReadOnlyRef, &colorReference, &depthReadOnlyRef);
	/* final; the depth for the translucent depth-only pass when the frame goes on */
	subpass(&subpasses[2], 1, &colorInput, &swapChainReference, &depthReference);

	/* (what reads the picture reads it anywhere: not by region, see vk_screen_drawer.c) */
	if (p->rtt)
		d = dep(d, 2, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0);
	else
		d = dep(d, 2, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0);
	d = dep(d, VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT);
	d = dep(d, VK_SUBPASS_EXTERNAL, 1, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT);
	/* (the two attachments, first used in subpasses 0 and 1) */
	for (s = 0; s < 2; s++)
		d = dep(d, VK_SUBPASS_EXTERNAL, s, oitStages, oitStages,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, 0);
	/* The picture's attachment is first used in subpass 2. With several
	 * render passes it was the polygons' colour attachment of the one
	 * before, written and then read as an input. */
	d = dep(d, VK_SUBPASS_EXTERNAL, 2,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	d = dep(d, 0, 1, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT);
	/* carries the dependency above on to where the colour attachment changes hands for subpass 2 */
	d = dep(d, 0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_DEPENDENCY_BY_REGION_BIT);
	/* the depth attachment is stored at the end of subpass 2, and changes layout after that - not before */
	d = dep(d, 2, VK_SUBPASS_EXTERNAL,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, oitStages,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, 0);
	/* (the colour attachment is stored at the end of subpass 2) */
	d = dep(d, 1, 2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_DEPENDENCY_BY_REGION_BIT);
	/* Only needed when the render pass is not the last, for the depth-only
	 * translucent pass; but every render pass is to be compatible, so all
	 * of them have it */
	d = dep(d, 1, 2, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT);
	d = dep(d, 2, 2, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT);

	return create(attachments, ARRAY_SIZE(attachments), subpasses, ARRAY_SIZE(subpasses), deps, (u32)(d - deps));
}

VkRenderPass vk_oit_passes_get(vk_oit_passes *p, bool initial, bool last, bool depthWrites, bool resumed)
{
	const unsigned index = (initial ? 1 : 0) | (last ? 2 : 0) | (depthWrites ? 4 : 0) | (resumed ? 8 : 0);
	if (p->passes[index] == VK_NULL_HANDLE)
		p->passes[index] = make(p, initial, last, depthWrites, resumed);
	return p->passes[index];
}

bool vk_oit_passes_make_all(vk_oit_passes *p)
{
	unsigned index;
	for (index = 0; index < ARRAY_SIZE(p->passes); index++)
		if (vk_oit_passes_get(p, (index & 1) != 0, (index & 2) != 0, (index & 4) != 0, (index & 8) != 0) == VK_NULL_HANDLE)
			return false;
	return true;
}

void vk_oit_passes_reset(vk_oit_passes *p)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(p->passes); i++)
		if (p->passes[i] != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(vkc.device, p->passes[i], NULL);
			p->passes[i] = VK_NULL_HANDLE;
		}
}

void vk_oit_passes_set_samples(vk_oit_passes *p, u32 samples)
{
	if (samples != p->samples)
	{
		vk_oit_passes_reset(p);
		p->samples = samples;
	}
}

void vk_oit_passes_set_keep_depth(vk_oit_passes *p, bool keep)
{
	if (keep != p->keepDepth)
	{
		vk_oit_passes_reset(p);
		p->keepDepth = keep;
	}
}
