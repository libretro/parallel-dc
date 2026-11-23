/* Textures made on the GPU from their bytes in video memory: see
 * vk_tex_decoder.h */
#include <stdlib.h>
#include <string.h>
#include "vk_tex_decoder.h"

/* The kernels, by TEXD_OUT8, 16, 32, as SPIR-V made from rend/texdecode.h
 * when the core was built: tools/rend/kernels_spv.sh, which checks that
 * they still are its. Nothing is compiled when the core runs. */
#include "rend/texdecode_spv.inc"

#define SAMPLED_LINEAR (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

/* An image of @format the kernel writes to through a @view view: optimal,
 * sampled, written and copied to and from */
static bool storable(VkFormat format, VkFormat view, bool extended)
{
	VkFormatProperties viewProps, props;
	VkImageFormatProperties ifp;
	const VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (extended ? VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0);
	vkGetPhysicalDeviceFormatProperties(vkc.gpu, view, &viewProps);
	if (!(viewProps.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
		return false;
	vkGetPhysicalDeviceFormatProperties(vkc.gpu, format, &props);
	if ((props.optimalTilingFeatures & SAMPLED_LINEAR) != SAMPLED_LINEAR)
		return false;
	return vkGetPhysicalDeviceImageFormatProperties(vkc.gpu, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			flags, &ifp) == VK_SUCCESS;
}

/* A format a planar texture's texels can be copied into as they are */
static VkFormat copyable(VkFormat format)
{
	VkFormatProperties props;
	const VkFormatFeatureFlags needed = SAMPLED_LINEAR | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	vkGetPhysicalDeviceFormatProperties(vkc.gpu, format, &props);
	return (props.optimalTilingFeatures & needed) == needed ? format : VK_FORMAT_UNDEFINED;
}

/* @key TEXD_OUT8, 16 or 32 */
static VkPipeline make_kernel(const vk_tex_decoder *d, u32 out, VkPipelineCache cache)
{
	VkComputePipelineCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	/* (run over a part of a level too: vk_texd_level::group) */
	info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = d->modules[out];
	info.stage.pName = "main";
	info.layout = d->pipelineLayout;
	return vk_pso_create_compute(vkc.device, cache, &info);
}

static VkPipeline build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	return make_kernel((const vk_tex_decoder *)self, (u32)key, cache);
}

void vk_tex_decoder_construct(vk_tex_decoder *d)
{
	memset(d, 0, sizeof(*d));
	d->builder.build = build;
	retro_atomic_int_init(&d->builder.generation, 0);
	PsoOwnInit(&d->builder, d->pipelines, 3);
}

static VkDescriptorSetLayout make_set_layout(VkDescriptorType type)
{
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo info;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = type;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 1;
	info.pBindings = &binding;
	if (vkCreateDescriptorSetLayout(vkc.device, &info, NULL, &layout) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return layout;
}

static VkDescriptorPool make_pool(VkDescriptorType type, u32 count)
{
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo info;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	size.type = type;
	size.descriptorCount = count;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.maxSets = count;
	info.poolSizeCount = 1;
	info.pPoolSizes = &size;
	if (vkCreateDescriptorPool(vkc.device, &info, NULL, &pool) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return pool;
}

static bool allocate_sets(VkDescriptorPool pool, const VkDescriptorSetLayout *layouts, u32 count, VkDescriptorSet *sets)
{
	VkDescriptorSetAllocateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = pool;
	info.descriptorSetCount = count;
	info.pSetLayouts = layouts;
	return vkAllocateDescriptorSets(vkc.device, &info, sets) == VK_SUCCESS;
}

/* @what could not be made: everything taken down */
static void failed(vk_tex_decoder *d, const char *what)
{
	ERROR_LOG(RENDERER, "Vulkan: %s for the texture decoder could not be made\n", what);
	vk_tex_decoder_term(d);
}

void vk_tex_decoder_init(vk_tex_decoder *d)
{
	const int swapChain = vkc.swapChainSize;
	/* the image formats the kernel writes through views of another format need Vulkan 1.1 */
	const bool v11 = VK_VERSION_MAJOR(vkc.apiVersion) > 1 || VK_VERSION_MINOR(vkc.apiVersion) >= 1;
	const bool extended = v11 && vkc.storageImageExtendedFormats;
	bool wanted[3];
	PsoSlot *asked[3];
	VkDescriptorSetLayout layouts[2];
	VkPushConstantRange pushRange;
	VkPipelineLayoutCreateInfo layoutInfo;
	VkDescriptorBufferInfo ringInfo;
	VkWriteDescriptorSet write;
	VkDescriptorSetLayout *setLayouts;
	u64 bytes, most;
	size_t arenaBytes;
	u8 *p;
	u32 n = 0, out;
	int i;

	if (d->ready && d->frames == swapChain)
		return;
	vk_tex_decoder_term(d);

#ifdef MSB_FIRST
	/* the kernel reads the bytes little-endian */
	if (true)
#else
	if (!v11)
#endif
	{
		INFO_LOG(RENDERER, "Vulkan: textures are converted on the CPU\n");
		return;
	}
	d->takes32 = storable(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT, false);
	d->takes16[0] = extended && storable(VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_R16_UINT, true);
	d->takes16[1] = extended && storable(VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R16_UINT, true);
	d->takes16[2] = extended && storable(VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_R16_UINT, true);
	d->takes8 = extended && storable(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UINT, true);
	d->native[0] = copyable(VK_FORMAT_A1R5G5B5_UNORM_PACK16);
	d->native[1] = copyable(VK_FORMAT_R5G6B5_UNORM_PACK16);
	d->native[2] = copyable(VK_FORMAT_B4G4R4A4_UNORM_PACK16);

	/* The ring: what a frame's textures take as they are in video memory -
	 * a few hundred KB in most frames, megabytes when a scene is loaded -
	 * a share of the video memory there is, between 2 and 16 MB a frame */
	bytes = (u64)vram_budget_available_kib(&vkc.vramBudget) * 1024 / 256;
	if (bytes < 2 * 1024 * 1024)
		bytes = 2 * 1024 * 1024;
	if (bytes > 16 * 1024 * 1024)
		bytes = 16 * 1024 * 1024;
	most = vkc.maxStorageBufferRange < vkc.maxMemoryAllocationSize ? vkc.maxStorageBufferRange : vkc.maxMemoryAllocationSize;
	if (bytes * swapChain > most)
		bytes = most / swapChain;
	d->ringFrame = (u32)bytes & ~15u;
	if (vk_buffer_init(&d->ring, (VkDeviceSize)d->ringFrame * swapChain,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
	{
		failed(d, "the ring");
		return;
	}
	d->ringMapped = (u8 *)vk_buffer_map(&d->ring);

	d->ringLayout = make_set_layout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	d->imageLayout = make_set_layout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	if (d->ringLayout == VK_NULL_HANDLE || d->imageLayout == VK_NULL_HANDLE)
	{
		failed(d, "a set layout");
		return;
	}
	layouts[0] = d->ringLayout;
	layouts[1] = d->imageLayout;
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(((vk_texd_level *)0)->pc);
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = layouts;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushRange;
	if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &d->pipelineLayout) != VK_SUCCESS)
	{
		d->pipelineLayout = VK_NULL_HANDLE;
		failed(d, "the pipeline layout");
		return;
	}

	/* every kernel there is, asked for now (of the pipeline thread): none is
	 * made while a game runs */
	wanted[0] = d->takes8;
	wanted[1] = d->takes16[0] || d->takes16[1] || d->takes16[2];
	wanted[2] = d->takes32;
	for (out = 0; out < 3; out++)
	{
		VkShaderModuleCreateInfo moduleInfo;
		if (!wanted[out])
			continue;
		memset(&moduleInfo, 0, sizeof(moduleInfo));
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = texdecode_spv_size[out];
		moduleInfo.pCode = texdecode_spv[out];
		if (vkCreateShaderModule(vkc.device, &moduleInfo, NULL, &d->modules[out]) != VK_SUCCESS)
		{
			d->modules[out] = VK_NULL_HANDLE;
			failed(d, "a kernel's module");
			return;
		}
		asked[n++] = &d->pipelines[out];
	}
	vk_pso_worker_build_soon(asked, n, vkc.pipelineCache);

	d->ringPool = make_pool(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1);
	if (d->ringPool == VK_NULL_HANDLE || !allocate_sets(d->ringPool, &layouts[0], 1, &d->ringSet))
	{
		failed(d, "the ring's descriptor set");
		return;
	}
	ringInfo.buffer = d->ring.buffer;
	ringInfo.offset = 0;
	ringInfo.range = VK_WHOLE_SIZE;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = d->ringSet;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &ringInfo;
	vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);

	/* the lists, sized once, in one piece */
	arenaBytes = VK_TEXD_MAX_JOBS * sizeof(vk_texd_job) + VK_TEXD_MAX_DISPATCHES * sizeof(vk_texd_level)
			+ VK_TEXD_MAX_JOBS * sizeof(vk_texd_copy) + (size_t)swapChain * VK_TEXD_MAX_DISPATCHES * sizeof(VkDescriptorSet)
			+ VK_TEXD_MAX_DISPATCHES * sizeof(VkDescriptorImageInfo) + VK_TEXD_MAX_DISPATCHES * sizeof(VkWriteDescriptorSet)
			+ VK_TEXD_MAX_JOBS * sizeof(VkImageMemoryBarrier) + (size_t)swapChain * sizeof(VkDescriptorPool)
			+ VK_TEXD_MAX_DISPATCHES * sizeof(VkDescriptorSetLayout);
	d->arena = calloc(1, arenaBytes);
	if (d->arena == NULL)
	{
		failed(d, "the lists");
		return;
	}
	/* (each piece's size a multiple of 8: every one aligned as it needs) */
	p = (u8 *)d->arena;
	d->sets = (VkDescriptorSet *)p;
	p += (size_t)swapChain * VK_TEXD_MAX_DISPATCHES * sizeof(VkDescriptorSet);
	d->writes = (VkWriteDescriptorSet *)p;
	p += VK_TEXD_MAX_DISPATCHES * sizeof(VkWriteDescriptorSet);
	d->barriers = (VkImageMemoryBarrier *)p;
	p += VK_TEXD_MAX_JOBS * sizeof(VkImageMemoryBarrier);
	d->imageInfos = (VkDescriptorImageInfo *)p;
	p += VK_TEXD_MAX_DISPATCHES * sizeof(VkDescriptorImageInfo);
	d->dispatches = (vk_texd_level *)p;
	p += VK_TEXD_MAX_DISPATCHES * sizeof(vk_texd_level);
	d->jobs = (vk_texd_job *)p;
	p += VK_TEXD_MAX_JOBS * sizeof(vk_texd_job);
	d->pools = (VkDescriptorPool *)p;
	p += (size_t)swapChain * sizeof(VkDescriptorPool);
	setLayouts = (VkDescriptorSetLayout *)p;
	p += VK_TEXD_MAX_DISPATCHES * sizeof(VkDescriptorSetLayout);
	d->copies = (vk_texd_copy *)p;

	for (i = 0; i < VK_TEXD_MAX_DISPATCHES; i++)
		setLayouts[i] = d->imageLayout;
	/* every set a frame's place can want, from its pool, now: written again
	 * each time round (the frame that last had the place done, its fence),
	 * none taken from the pool while the game runs */
	d->frames = swapChain;
	for (i = 0; i < swapChain; i++)
	{
		d->pools[i] = make_pool(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_TEXD_MAX_DISPATCHES);
		if (d->pools[i] == VK_NULL_HANDLE
				|| !allocate_sets(d->pools[i], setLayouts, VK_TEXD_MAX_DISPATCHES, &d->sets[(size_t)i * VK_TEXD_MAX_DISPATCHES]))
		{
			ERROR_LOG(RENDERER, "Vulkan: no descriptor sets for the texture decoder\n");
			vk_tex_decoder_term(d);
			return;
		}
	}

	d->index = 0;
	d->jobCount = d->dispatchCount = d->copyCount = 0;
	d->ringUsed = 0;
	d->ready = true;
	INFO_LOG(RENDERER, "Vulkan: textures are made on the GPU (%u KB of ring a frame; 32-bit %d, 16-bit %d%d%d, 8-bit %d; copied as they are %d%d%d)\n",
			d->ringFrame / 1024, d->takes32, d->takes16[0], d->takes16[1], d->takes16[2], d->takes8,
			d->native[0] != VK_FORMAT_UNDEFINED, d->native[1] != VK_FORMAT_UNDEFINED, d->native[2] != VK_FORMAT_UNDEFINED);
}

void vk_tex_decoder_term(vk_tex_decoder *d)
{
	int i;
	d->ready = false;
	d->jobCount = d->dispatchCount = d->copyCount = 0;
	if (d->pools != NULL)
		for (i = 0; i < d->frames; i++)
			if (d->pools[i] != VK_NULL_HANDLE)
				vkDestroyDescriptorPool(vkc.device, d->pools[i], NULL);
	free(d->arena);
	d->arena = NULL;
	d->pools = NULL;
	d->jobs = NULL;
	d->dispatches = NULL;
	d->copies = NULL;
	d->sets = NULL;
	d->imageInfos = NULL;
	d->writes = NULL;
	d->barriers = NULL;
	d->ringSet = VK_NULL_HANDLE;
	if (d->ringPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(vkc.device, d->ringPool, NULL);
	d->ringPool = VK_NULL_HANDLE;
	/* (the pipeline thread done with what it has of them before the modules go) */
	PsoOwnDrop(&d->builder, d->pipelines, 3, vkc.device);
	for (i = 0; i < 3; i++)
	{
		if (d->modules[i] != VK_NULL_HANDLE)
			vkDestroyShaderModule(vkc.device, d->modules[i], NULL);
		d->modules[i] = VK_NULL_HANDLE;
	}
	if (d->pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vkc.device, d->pipelineLayout, NULL);
	if (d->imageLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, d->imageLayout, NULL);
	if (d->ringLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, d->ringLayout, NULL);
	d->pipelineLayout = VK_NULL_HANDLE;
	d->imageLayout = d->ringLayout = VK_NULL_HANDLE;
	d->ringMapped = NULL;
	vk_buffer_free(&d->ring);
	d->frames = 0;
	d->takes32 = d->takes8 = false;
	d->takes16[0] = d->takes16[1] = d->takes16[2] = false;
	d->native[0] = d->native[1] = d->native[2] = VK_FORMAT_UNDEFINED;
}

/* (one the pipeline thread could not make: made here; VK_NULL_HANDLE if
 * it cannot be) */
static VkPipeline pipeline(vk_tex_decoder *d, u32 out)
{
	const VkPipeline made = vk_pso_get(&d->pipelines[out]);
	if (made != VK_NULL_HANDLE)
		return made;
	return PsoOwnMade(&d->pipelines[out], make_kernel(d, out, vkc.pipelineCache));
}

bool vk_tex_decoder_takes(const vk_tex_decoder *d, VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_R8G8B8A8_UNORM: return d->takes32;
	case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return d->takes16[0];
	case VK_FORMAT_R5G6B5_UNORM_PACK16: return d->takes16[1];
	case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return d->takes16[2];
	case VK_FORMAT_R8_UNORM: return d->takes8;
	default: return false;
	}
}

VkFormat vk_tex_decoder_native_format(const vk_tex_decoder *d, int type)
{
	switch (type)
	{
	case TEX_TYPE_5551: return d->native[0];
	case TEX_TYPE_565: return d->native[1];
	case TEX_TYPE_4444: return d->native[2];
	default: return VK_FORMAT_UNDEFINED;
	}
}

bool vk_tex_decoder_room(const vk_tex_decoder *d, u32 bytes, u32 count)
{
	if (!d->ready || d->flushed || d->jobCount >= VK_TEXD_MAX_JOBS || d->dispatchCount + count > VK_TEXD_MAX_DISPATCHES)
		return false;
	return d->ringUsed + ((bytes + 15) & ~15u) <= d->ringFrame;
}

void vk_tex_decoder_begin_frame(vk_tex_decoder *d, int frame)
{
	if (!d->ready)
		return;
	d->index = frame % d->frames;
	d->serial++;
	d->flushed = false;
	d->jobCount = d->dispatchCount = d->copyCount = 0;
	d->ringUsed = 0;
	/* (the frame that last had this place is done, its fence: so are its sets) */
}

u8 *vk_tex_decoder_reserve(vk_tex_decoder *d, u32 bytes, u32 *offset)
{
	bytes = (bytes + 15) & ~15u;
	if (!d->ready || d->ringUsed + bytes > d->ringFrame)
	{
		d->statFull++;
		return NULL;
	}
	*offset = (u32)d->index * d->ringFrame + d->ringUsed;
	d->ringUsed += bytes;
	d->statBytes += bytes;
	return d->ringMapped + *offset;
}

int vk_tex_decoder_add_decode(vk_tex_decoder *d, VkImage image, bool fresh, u32 mipLevels, u32 out,
		const vk_texd_level *levels, u32 count)
{
	vk_texd_job *job = &d->jobs[d->jobCount];
	job->image = image;
	job->first = d->dispatchCount;
	job->count = count;
	job->levels = mipLevels;
	job->out = out;
	job->fresh = fresh;
	job->copy = false;
	job->cancelled = false;
	memcpy(&d->dispatches[d->dispatchCount], levels, count * sizeof(vk_texd_level));
	d->dispatchCount += count;
	d->statTextures++;
	d->statDispatches += count;
	return (int)d->jobCount++;
}

int vk_tex_decoder_add_copy(vk_tex_decoder *d, VkImage image, bool fresh, u32 offset, u32 rowLength,
		u32 width, u32 height, u32 y)
{
	vk_texd_job *job = &d->jobs[d->jobCount];
	vk_texd_copy *copy = &d->copies[d->copyCount];
	job->image = image;
	job->first = d->copyCount++;
	job->count = 0;
	job->levels = 1;
	job->out = TEXD_OUT16;
	job->fresh = fresh;
	job->copy = true;
	job->cancelled = false;
	copy->offset = offset;
	copy->rowLength = rowLength;
	copy->width = width;
	copy->height = height;
	copy->y = y;
	d->statTextures++;
	d->statCopies++;
	return (int)d->jobCount++;
}

bool vk_tex_decoder_cancel(vk_tex_decoder *d, int job)
{
	if (job < 0 || (u32)job >= d->jobCount || d->jobs[job].cancelled)
		return false;
	d->jobs[job].cancelled = true;
	return d->jobs[job].fresh;
}

bool vk_tex_decoder_flush(vk_tex_decoder *d, vk_cmd_pool *pool)
{
	u32 live = 0, liveDispatches = 0, n = 0, b = 0, i, out;
	VkDescriptorSet *frameSets;
	VkCommandBuffer cmd;
	VkCommandBufferBeginInfo begin;
	bool made = true;

	if (!d->ready)
		return true;
	/* Nothing more until the next frame: what was not recorded here would
	 * not be done. And what textures know of their jobs is out of date. */
	d->flushed = true;
	d->serial++;
	for (i = 0; i < d->jobCount; i++)
		if (!d->jobs[i].cancelled)
		{
			live++;
			liveDispatches += d->jobs[i].count;
		}
	if (live == 0)
	{
		d->jobCount = d->dispatchCount = d->copyCount = 0;
		return true;
	}

	/* a storage image set for each dispatch, all in one go */
	frameSets = &d->sets[(size_t)d->index * VK_TEXD_MAX_DISPATCHES];
	if (liveDispatches > 0)
	{
		for (i = 0; i < d->jobCount; i++)
		{
			const vk_texd_job *job = &d->jobs[i];
			u32 k;
			if (job->cancelled)
				continue;
			for (k = job->first; k < job->first + job->count; k++)
			{
				VkWriteDescriptorSet *w = &d->writes[n];
				d->imageInfos[n].sampler = VK_NULL_HANDLE;
				d->imageInfos[n].imageView = d->dispatches[k].view;
				d->imageInfos[n].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				memset(w, 0, sizeof(*w));
				w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				w->dstSet = frameSets[n];
				w->descriptorCount = 1;
				w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				w->pImageInfo = &d->imageInfos[n];
				n++;
			}
		}
		vkUpdateDescriptorSets(vkc.device, n, d->writes, 0, NULL);
	}

	cmd = vk_cmd_pool_allocate(pool);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	/* Every image from what it was - sampled by an earlier frame, or never
	 * written - to what it is written as. Nothing in them is kept. */
	for (i = 0; i < d->jobCount; i++)
	{
		const vk_texd_job *job = &d->jobs[i];
		VkImageMemoryBarrier *barrier = &d->barriers[b];
		if (job->cancelled)
			continue;
		memset(barrier, 0, sizeof(*barrier));
		barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier->dstAccessMask = job->copy ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT;
		barrier->oldLayout = job->fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier->newLayout = job->copy ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
		barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier->image = job->image;
		barrier->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier->subresourceRange.levelCount = job->levels;
		barrier->subresourceRange.layerCount = 1;
		b++;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, b, d->barriers);

	for (i = 0; i < d->jobCount; i++)
	{
		const vk_texd_job *job = &d->jobs[i];
		const vk_texd_copy *c;
		VkBufferImageCopy region;
		if (job->cancelled || !job->copy)
			continue;
		c = &d->copies[job->first];
		memset(&region, 0, sizeof(region));
		region.bufferOffset = c->offset;
		region.bufferRowLength = c->rowLength;
		region.bufferImageHeight = c->height;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageOffset.y = (int32_t)c->y;
		region.imageExtent.width = c->width;
		region.imageExtent.height = c->height;
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(cmd, d->ring.buffer, job->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	if (n > 0)
	{
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipelineLayout, 0, 1, &d->ringSet, 0, NULL);
		/* a kernel at a time, in the order the sets were written */
		for (out = 0; out < 3; out++)
		{
			VkPipeline kernel = VK_NULL_HANDLE;
			bool bound = false;
			u32 s = 0;
			for (i = 0; i < d->jobCount; i++)
			{
				const vk_texd_job *job = &d->jobs[i];
				u32 k;
				if (job->cancelled)
					continue;
				if (job->out != out)
				{
					s += job->count;
					continue;
				}
				if (!bound && job->count > 0)
				{
					kernel = pipeline(d, out);
					if (kernel == VK_NULL_HANDLE)
						made = false;
					else
						vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel);
					bound = true;
				}
				for (k = job->first; k < job->first + job->count; k++, s++)
				{
					const vk_texd_level *lv = &d->dispatches[k];
					if (kernel == VK_NULL_HANDLE)
						continue;
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipelineLayout, 1, 1, &frameSets[s], 0, NULL);
					vkCmdPushConstants(cmd, d->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lv->pc), lv->pc);
					if (lv->group[0] == 0 && lv->group[1] == 0)
						vkCmdDispatch(cmd, lv->group[2], lv->group[3], 1);
					else
						vkCmdDispatchBase(cmd, lv->group[0], lv->group[1], 0, lv->group[2], lv->group[3], 1);
				}
			}
		}
	}

	/* and to be sampled by this frame's draws */
	for (i = 0; i < b; i++)
	{
		VkImageMemoryBarrier *barrier = &d->barriers[i];
		barrier->srcAccessMask = barrier->dstAccessMask;
		barrier->dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier->oldLayout = barrier->newLayout;
		barrier->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, b, d->barriers);
	vkEndCommandBuffer(cmd);

	d->jobCount = d->dispatchCount = d->copyCount = 0;
	return made;
}
