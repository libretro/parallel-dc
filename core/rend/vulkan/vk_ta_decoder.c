/* GPU-side TA processing on Vulkan: see vk_ta_decoder.h */
#include <stdlib.h>
#include <string.h>
#include <features/features_cpu.h>
#include "vk_ta_decoder.h"
#include "vk_buffer.h"
#include "pso.h"
#include "rend/ta_gpu.h"

extern int screen_height;

/* The kernels, by TAG_K_*, as SPIR-V made from rend/ta_gpu.h when the core
 * was built: tools/rend/kernels_spv.sh, which checks that they still are
 * its. Nothing is compiled when the core runs. */
#include "rend/ta_gpu_spv.inc"
/* (a kernel for each TAG_K_*) */
typedef char vk_ta_kernels_check[sizeof(ta_gpu_spv) / sizeof(ta_gpu_spv[0]) == TAG_KERNELS + 1 ? 1 : -1];
/* (the drawers draw from the records and commands: a record is an indexed
 * indirect draw) */
typedef char vk_ta_command_check[TAG_C_WORDS * 4 == sizeof(VkDrawIndexedIndirectCommand) ? 1 : -1];
/* (fZ_max is the frame's first word: Record()'s fills) */
typedef char vk_ta_zmax_check[TAG_O_ZMAX == 0 ? 1 : -1];

/* The buffers, in the kernels' bindings' order (set 1); then the frame's
 * piece of the sorted indices (B_SRT: the prepass's kernel's) */
enum { B_TAB, B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_COUNT, B_SRT = B_COUNT };

#define VK_TA_MAX_SLICES 16

/* (its kernels' pipelines: asked of the pipeline thread by init(), pso.h;
 * the first frame takes them, or makes them there and fails there as
 * init() did when the device cannot). One allocation, with the frame
 * slots' arrays after it; given to the texture pool to free once the
 * frames that may still read its buffers are done. */
typedef struct vk_ta_res
{
	vk_pso_builder builder;
	VkShaderModule modules[TAG_KERNELS + 1];
	PsoSlot kernels[TAG_KERNELS + 1];

	vk_buffer pool;			/* the TA contexts' data */
	vk_buffer buffers[B_COUNT];
	u32 *tables;			/* mapped: a piece for each frame */
	u32 tableWords;
	u32 slice, slices;
	TaGpuLayout layout;
	VkDescriptorSetLayout setLayouts[2];
	VkPipelineLayout pipelineLayout;
	VkDescriptorPool descriptorPool;
	VkDescriptorSet sliceSets[VK_TA_MAX_SLICES];
	VkDescriptorSet *tableSets;	/* a frame's each */
	VkQueryPool queries;		/* two timestamps a frame, if the queue has them */
	double period;			/* ns a tick */
	u64 mask;
	/* (TA_GPU_SORT) room for the sorted translucent indices of a frame,
	 * for each slot of the texture pool, whose command buffers the drawers
	 * draw with: a slot's are written by the CPU once begin_frame() has
	 * waited for the slot's fence, which the draws of the frame that last
	 * read them were submitted with */
	vk_buffer sorted;
	u8 *sortedMap;
	VkDeviceSize sortSlot;		/* bytes; 0: none */
	u32 *slotSerial;		/* the frame each slot of the pool last had */
	bool *slotTimed;		/* ...and whether it has a decode timed */
} vk_ta_res;

static VkPipeline make(const vk_ta_res *r, u64 k, VkPipelineCache cache)
{
	VkComputePipelineCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = r->modules[k];
	info.stage.pName = "main";
	info.layout = r->pipelineLayout;
	return vk_pso_create_compute(vkc.device, cache, &info);
}

/* the pipeline thread's */
static VkPipeline build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	return make((const vk_ta_res *)self, key, cache);
}

/* (one the pipeline thread could not make: made here, or none, as init()
 * would have failed) */
static VkPipeline kernel(vk_ta_res *r, int k)
{
	const VkPipeline made = vk_pso_get(&r->kernels[k]);
	if (made != VK_NULL_HANDLE)
		return made;
	return PsoOwnMade(&r->kernels[k], make(r, (u64)k, vkc.pipelineCache));
}

static void res_free(void *object)
{
	vk_ta_res *r = (vk_ta_res *)object;
	unsigned i;
	/* (the pipeline thread done with what it has of them before the modules go) */
	PsoOwnDrop(&r->builder, r->kernels, TAG_KERNELS + 1, vkc.device);
	vk_buffer_free(&r->sorted);
	if (r->queries != VK_NULL_HANDLE)
		vkDestroyQueryPool(vkc.device, r->queries, NULL);
	/* (the sets with it) */
	if (r->descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(vkc.device, r->descriptorPool, NULL);
	if (r->pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vkc.device, r->pipelineLayout, NULL);
	for (i = 0; i < 2; i++)
		if (r->setLayouts[i] != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, r->setLayouts[i], NULL);
	for (i = 0; i < B_COUNT; i++)
		vk_buffer_free(&r->buffers[i]);
	vk_buffer_free(&r->pool);
	for (i = 0; i <= TAG_KERNELS; i++)
		if (r->modules[i] != VK_NULL_HANDLE)
			vkDestroyShaderModule(vkc.device, r->modules[i], NULL);
	free(r);
}

void vk_ta_decoder_construct(vk_ta_decoder *d)
{
	memset(d, 0, sizeof(*d));
}

/* @size bytes of @usage, in @props; false, and said, if not */
static bool make_buffer(vk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props)
{
	const VkResult res = vk_buffer_init(b, size, usage, props);
	if (res == VK_SUCCESS)
		return true;
	INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (vk_buffer_init: %d)\n", (int)res);
	return false;
}

static void set_binding(VkDescriptorSetLayoutBinding *b, u32 binding)
{
	memset(b, 0, sizeof(*b));
	b->binding = binding;
	b->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	b->descriptorCount = 1;
	b->stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
}

static void set_write(VkWriteDescriptorSet *w, VkDescriptorBufferInfo *info, VkDescriptorSet set, u32 binding,
		VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
{
	info->buffer = buffer;
	info->offset = offset;
	info->range = range;
	memset(w, 0, sizeof(*w));
	w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w->dstSet = set;
	w->dstBinding = binding;
	w->descriptorCount = 1;
	w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	w->pBufferInfo = info;
}

/* The descriptor sets: one for each slice of the contexts' data, and one
 * for each frame, of its tables and the rest */
static bool make_sets(vk_ta_res *r, int frames)
{
	VkDescriptorPoolSize poolSize;
	VkDescriptorPoolCreateInfo poolInfo;
	VkDescriptorSetAllocateInfo allocInfo;
	VkDescriptorSetLayout layouts[VK_TA_MAX_SLICES];
	VkDescriptorBufferInfo *infos;
	VkWriteDescriptorSet *writes;
	const u32 count = r->slices + (B_COUNT + 1) * (u32)frames;
	u32 n = 0, i;
	int f, b;

	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = count;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = r->slices + (u32)frames;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	if (vkCreateDescriptorPool(vkc.device, &poolInfo, NULL, &r->descriptorPool) != VK_SUCCESS)
		return false;
	memset(&allocInfo, 0, sizeof(allocInfo));
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = r->descriptorPool;
	allocInfo.descriptorSetCount = r->slices;
	allocInfo.pSetLayouts = layouts;
	for (i = 0; i < r->slices; i++)
		layouts[i] = r->setLayouts[0];
	if (vkAllocateDescriptorSets(vkc.device, &allocInfo, r->sliceSets) != VK_SUCCESS)
		return false;
	/* (a set at a time: a frame's count is the swapchain's) */
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &r->setLayouts[1];
	for (f = 0; f < frames; f++)
		if (vkAllocateDescriptorSets(vkc.device, &allocInfo, &r->tableSets[f]) != VK_SUCCESS)
			return false;

	/* (made when the decoder is, never while a game runs) */
	infos = (VkDescriptorBufferInfo *)malloc(count * sizeof(*infos));
	writes = (VkWriteDescriptorSet *)malloc(count * sizeof(*writes));
	if (infos == NULL || writes == NULL)
	{
		free(infos);
		free(writes);
		return false;
	}
	for (i = 0; i < r->slices; i++, n++)
		set_write(&writes[n], &infos[n], r->sliceSets[i], 0, r->pool.buffer, (VkDeviceSize)i * r->slice, r->slice);
	for (f = 0; f < frames; f++)
		for (b = 0; b < B_COUNT; b++, n++)
		{
			if (b == B_TAB)
				set_write(&writes[n], &infos[n], r->tableSets[f], (u32)b, r->buffers[b].buffer,
						(VkDeviceSize)f * r->tableWords * 4, (VkDeviceSize)r->tableWords * 4);
			else
				set_write(&writes[n], &infos[n], r->tableSets[f], (u32)b, r->buffers[b].buffer, 0, VK_WHOLE_SIZE);
		}
	/* (the frame's sorted indices; without them, which no kernel then reads, the frame buffer stands in) */
	for (f = 0; f < frames; f++, n++)
	{
		if (r->sortSlot != 0)
			set_write(&writes[n], &infos[n], r->tableSets[f], B_SRT, r->sorted.buffer, (VkDeviceSize)f * r->sortSlot, r->sortSlot);
		else
			set_write(&writes[n], &infos[n], r->tableSets[f], B_SRT, r->buffers[B_FRM].buffer, 0, VK_WHOLE_SIZE);
	}
	vkUpdateDescriptorSets(vkc.device, n, writes, 0, NULL);
	free(infos);
	free(writes);
	return true;
}

/* everything but the buffers and the sets: the layouts, the kernels asked
 * for, the timestamps */
static bool make_kernels(vk_ta_res *r, int frames)
{
	VkDescriptorSetLayoutBinding bindings[B_COUNT + 1];
	VkDescriptorSetLayoutCreateInfo setInfo;
	VkPipelineLayoutCreateInfo layoutInfo;
	PsoSlot *asked[TAG_KERNELS];
	VkQueueFamilyProperties *families;
	u32 familyCount = 0, bits = 0;
	const u32 family = vkc.iface->queue_index;
	int i, k;

	for (i = 0; i <= B_COUNT; i++)
		set_binding(&bindings[i], (u32)i);
	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 1;
	setInfo.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &r->setLayouts[0]) != VK_SUCCESS)
		return false;
	setInfo.bindingCount = B_COUNT + 1;
	if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &r->setLayouts[1]) != VK_SUCCESS)
		return false;
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = r->setLayouts;
	if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &r->pipelineLayout) != VK_SUCCESS)
		return false;

	/* every kernel, asked for now (of the pipeline thread): none is made while a game runs */
	for (k = 1; k <= TAG_KERNELS; k++)
	{
		VkShaderModuleCreateInfo moduleInfo;
		memset(&moduleInfo, 0, sizeof(moduleInfo));
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = ta_gpu_spv_size[k];
		moduleInfo.pCode = ta_gpu_spv[k];
		if (vkCreateShaderModule(vkc.device, &moduleInfo, NULL, &r->modules[k]) != VK_SUCCESS)
			return false;
		asked[k - 1] = &r->kernels[k];
	}
	vk_pso_worker_build_soon(asked, TAG_KERNELS, vkc.pipelineCache);

	if (!make_sets(r, frames))
		return false;

	/* GPU time, read when a frame is known to be done: never waited for */
	vkGetPhysicalDeviceQueueFamilyProperties(vkc.gpu, &familyCount, NULL);
	families = (VkQueueFamilyProperties *)malloc((familyCount ? familyCount : 1) * sizeof(*families));
	if (families != NULL)
	{
		vkGetPhysicalDeviceQueueFamilyProperties(vkc.gpu, &familyCount, families);
		bits = family < familyCount ? families[family].timestampValidBits : 0;
		free(families);
	}
	if (bits != 0)
	{
		VkQueryPoolCreateInfo queryInfo;
		VkPhysicalDeviceProperties props;
		memset(&queryInfo, 0, sizeof(queryInfo));
		queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryInfo.queryCount = 2 * (u32)frames;
		if (vkCreateQueryPool(vkc.device, &queryInfo, NULL, &r->queries) != VK_SUCCESS)
			return false;
		vkGetPhysicalDeviceProperties(vkc.gpu, &props);
		r->period = props.limits.timestampPeriod;
		r->mask = bits >= 64 ? ~(u64)0 : ((u64)1 << bits) - 1;
	}
	return true;
}

/* the buffers: false, and said, if one could not be made */
static bool make_buffers(vk_ta_res *r, const TaGpuSizes *sz, int frames, u32 strips, u32 workWords, u32 trdSlots, bool sortWanted)
{
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

	if (!make_buffer(&r->pool, (VkDeviceSize)r->slice * r->slices, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host))
		return false;
	/* The SH4 reads the control words back and states copy the data:
	 * memory without the host's cache in front of it is too slow for that */
	if (!(vk_buffer_memory_flags(&r->pool) & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
	{
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (no host-cached memory)\n");
		return false;
	}
	if (!make_buffer(&r->buffers[B_TAB], (VkDeviceSize)r->tableWords * 4 * frames, storage, host))
		return false;
	r->tables = (u32 *)vk_buffer_map(&r->buffers[B_TAB]);
	if (!make_buffer(&r->buffers[B_VTX], (VkDeviceSize)sz->verts * TAG_V_WORDS * 4, storage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, local)
			|| !make_buffer(&r->buffers[B_MOD], (VkDeviceSize)sz->modtrig * 9 * 4 + 4, storage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, local)
			|| !make_buffer(&r->buffers[B_IDX], (VkDeviceSize)sz->idx * 4, storage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, local)
			|| !make_buffer(&r->buffers[B_WORK], (VkDeviceSize)workWords * 4, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, local)
			|| !make_buffer(&r->buffers[B_REC], (VkDeviceSize)strips * TAG_C_WORDS * 4, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, local)
			/* (and a second command for each, for shadows: TAG_T_SHD_CMD) */
			|| !make_buffer(&r->buffers[B_CMD], (VkDeviceSize)sz->headers * 2 * TAG_C_WORDS * 4,
					storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, local)
			/* (read by a copy too: the drawers take 1/fZ_max from it, vk_ta_gpu_draw::frame) */
			|| !make_buffer(&r->buffers[B_FRM], TAG_O_WORDS * 4, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
					| VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, local)
			|| !make_buffer(&r->buffers[B_TRD], (VkDeviceSize)trdSlots * TAG_C_WORDS * 4,
					storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, local)
			|| !make_buffer(&r->buffers[B_TRP], (VkDeviceSize)sz->list[2] * TAG_P_WORDS * 4, storage, local))
		return false;
	/* The sorted indices: three a triangle, for as many triangles as a
	 * frame can have indices, and as many again for the depth prepass
	 * (rend/sorter.h), whose triangles are some of those, and its
	 * checks after them (TAG_T_PREP). In host-visible memory, which the
	 * CPU writes them to once (they are its sort's) and the GPU reads
	 * once - but for the prepass's triangles the prepass's kernel makes
	 * nothing of (TAG_K_PREPASS). */
	if (sortWanted)
	{
		const VkDeviceSize slot = ((VkDeviceSize)sz->idx * 2 * 3 * 4 + 255) & ~(VkDeviceSize)255;
		if (!make_buffer(&r->sorted, slot * frames, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host))
			return false;
		r->sortedMap = (u8 *)vk_buffer_map(&r->sorted);
		r->sortSlot = slot;
	}
	return true;
}

static bool init(vk_ta_decoder *d, vk_cmd_pool *pool)
{
	TaGpuSizes sz;
	vk_ta_res *r;
	u32 work[9];
	u32 strips, workWords, spares, trdSlots;
	u64 budget, most;
	int i, slices;

#ifdef MSB_FIRST
	/* (the kernels read the blocks little-endian) */
	(void)pool;
	d->failed = true;
	return false;
#else
	d->frames = (int)pool->slotCount;
	ta_gpu_sizes(&sz);
	strips = sz.list[0] + sz.list[1] + sz.list[2];
	workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	/* the translucent draw records: each group's count and room for a draw a vertex */
	trdSlots = sz.verts + sz.headers;

	most = vkc.maxStorageBufferRange < vkc.maxMemoryAllocationSize ? vkc.maxStorageBufferRange : vkc.maxMemoryAllocationSize;
	if ((u64)ta_data_size() > most || (u64)sz.verts * TAG_V_WORDS * 4 > most || (u64)workWords * 4 > most
			|| (u64)trdSlots * TAG_C_WORDS * 4 > most)
	{
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (buffers larger than the device has)\n");
		d->failed = true;
		return false;
	}
	/* A translucent draw group is one indirect draw of all its strips: with
	 * their count from the GPU (VK_KHR_draw_indirect_count), or of the
	 * group's whole room, whose records past the count draw nothing */
	d->draw.drawCount = vkc.cmdDrawIndexedIndirectCount;
	if ((vkc.multiDrawIndirect ? vkc.maxDrawIndirectCount : 1) < sz.verts || (d->draw.drawCount == NULL && !vkc.multiDrawIndirect))
	{
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (no indirect draws of several at once)\n");
		d->failed = true;
		return false;
	}
	d->instance = vkc.drawIndirectFirstInstance;

	r = (vk_ta_res *)calloc(1, sizeof(*r) + (size_t)d->frames * (sizeof(VkDescriptorSet) + sizeof(u32) + sizeof(bool)));
	if (r == NULL)
	{
		d->failed = true;
		return false;
	}
	r->builder.build = build;
	retro_atomic_int_init(&r->builder.generation, 0);
	PsoOwnInit(&r->builder, r->kernels, TAG_KERNELS + 1);
	r->tableSets = (VkDescriptorSet *)(r + 1);
	r->slotSerial = (u32 *)(r->tableSets + d->frames);
	r->slotTimed = (bool *)(r->slotSerial + d->frames);
	/* The contexts: every frame in flight, one being drawn, and the few the
	 * game writes to at a time and renders to textures with */
	r->slice = ta_data_size();
	/* ...and spares, for a game that writes to more at once, as many as a
	 * thirty-second of the memory budget left allows, up to four more: a
	 * frame that finds them all taken is drawn from the CPU's parse, in a
	 * context of its own memory (tactx_gpu_pool_attach() makes one) */
	budget = (u64)vram_budget_available_kib(&vkc.vramBudget) * 1024 / 32;
	spares = budget / r->slice < 4 ? (u32)(budget / r->slice) : 4;
	slices = d->frames + 4 + (int)spares;
	r->slices = (u32)(slices < VK_TA_MAX_SLICES ? slices : VK_TA_MAX_SLICES);
	r->tableWords = (sz.table_words + 63) & ~63u;

	if (!make_buffers(r, &sz, d->frames, strips, workWords, trdSlots, d->sortWanted))
	{
		res_free(r);
		d->failed = true;
		return false;
	}

	r->layout.words[0] = 0;		/* (each slice is bound from its start) */
	for (i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;

	if (!make_kernels(r, d->frames))
	{
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (its kernels could not be set up)\n");
		res_free(r);
		d->failed = true;
		return false;
	}

	if (!tactx_gpu_pool_attach((u8 *)vk_buffer_map(&r->pool), r->slice, r->slices))
	{
		res_free(r);
		d->failed = true;
		return false;
	}
	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
	{
		tactx_gpu_pool_detach();
		res_free(r);
		d->failed = true;
		return false;
	}
	/* (without the CPU's room to sort in, a frame with a sorted pass is parsed and sorted as ever) */
	if (r->sortSlot != 0 && !ta_gpu_sort_reserve(true))
		r->sortSlot = 0;
	d->draw.vertices = r->buffers[B_VTX].buffer;
	d->draw.indices = r->buffers[B_IDX].buffer;
	d->draw.modtrig = r->buffers[B_MOD].buffer;
	d->draw.commands = r->buffers[B_CMD].buffer;
	d->draw.trDraws = r->buffers[B_TRD].buffer;
	d->draw.trParams = r->buffers[B_TRP].buffer;
	d->draw.frame = r->buffers[B_FRM].buffer;
	d->draw.trParamsSize = (VkDeviceSize)sz.list[2] * TAG_P_WORDS * 4;
	d->draw.shadowCommands = (VkDeviceSize)sz.headers * TAG_C_WORDS * 4;
	d->draw.sorted = r->sorted.buffer;
	d->draw.sortedOffset = 0;
	d->draw.sortedMap = NULL;
	d->sortable = r->sortSlot != 0;
	d->res = r;
	d->ready = true;
	NOTICE_LOG(RENDERER, "Vulkan: TA processing on the GPU (%u frames of %u MB in host-cached memory, %u KB of tables a frame; "
			"translucent groups drawn %s, %s strip numbers%s)\n",
			r->slices, r->slice >> 20, r->tableWords / 256, d->draw.drawCount != NULL ? "with their count" : "whole",
			d->instance ? "with" : "without", r->sortSlot != 0 ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
#endif
}

static void say(const vk_ta_decoder *d, const char *when)
{
	u32 shortFrames, allocated;
	tactx_gpu_pool_stats(&shortFrames, &allocated);
	NOTICE_LOG(RENDERER, "Vulkan: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, %llu might run out of room); "
			"GPU time mean %.3f ms, max %.3f ms; pool: %u frames found it full, %u contexts allocated; "
			"%llu with a sorted pass (the sort mean %.1f us, max %.1f us), %llu with shadows the GPU decides\n",
			when, (unsigned long long)d->statFrames, (unsigned long long)d->statSkipped,
			(unsigned long long)d->statWhy[TA_GPU_WHY_TABLE], (unsigned long long)d->statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)d->statWhy[TA_GPU_WHY_SORT], (unsigned long long)d->statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)d->statWhy[TA_GPU_WHY_ROOM],
			d->statGpuTimed ? d->statGpuMs / d->statGpuTimed : 0.0, d->statGpuMaxMs, shortFrames, allocated,
			(unsigned long long)d->statSorted, d->statSorted ? d->statSortUs / d->statSorted : 0.0, d->statSortMaxUs,
			(unsigned long long)d->statShadowed);
}

void vk_ta_decoder_term(vk_ta_decoder *d)
{
	if (!d->ready)
		return;
	say(d, "");
	/* (the renderer is done with the GPU) */
	ta_set_gpu_processing(false);
	tactx_gpu_pool_detach();
	res_free(d->res);
	d->res = NULL;
	d->ready = false;
	d->sortable = false;
	d->prepared = false;
}

/* Every frame up to the newest whose slot's fence has signalled is done:
 * a fence is signalled once all that was submitted before it is */
static void poll_done(vk_ta_decoder *d, vk_cmd_pool *pool)
{
	int s;
	for (s = 0; s < d->frames; s++)
		if (s != d->index && (s32)(d->res->slotSerial[s] - d->done) > 0 && vk_cmd_pool_is_done(pool, s))
			d->done = d->res->slotSerial[s];
	tactx_gpu_done(d->done);
}

void vk_ta_decoder_setup(vk_ta_decoder *d, vk_cmd_pool *pool, bool wanted)
{
	retro_time_t start;
	if (d->ready && wanted && d->frames == (int)pool->slotCount)
		return;
	if (d->ready)
	{
		/* Turned off while a game runs, or another size of the texture
		 * pool: the frames in the pool go on in memory of their own, and
		 * the buffers go once the frames that may still read them are done */
		ta_set_gpu_processing(false);
		tactx_gpu_pool_detach();
		vk_cmd_pool_defer(pool, d->res, res_free);
		d->res = NULL;
		d->ready = false;
		d->sortable = false;
		d->prepared = false;
	}
	if (!wanted)
	{
		d->failed = false;
		return;
	}
	if (d->failed)
		return;
	start = cpu_features_get_time_usec();
	if (init(d, pool))
		NOTICE_LOG(RENDERER, "Vulkan: TA processing set up in %.1f ms\n", (double)(cpu_features_get_time_usec() - start) / 1000.0);
}

void vk_ta_decoder_begin_frame(vk_ta_decoder *d, vk_cmd_pool *pool)
{
	vk_ta_res *r = d->res;
	/* (made in setup(), never here: a frame the decoder is not ready for
	 * is parsed on the CPU) */
	if (!d->ready)
		return;
	if (d->frames != (int)pool->slotCount)
	{
		vk_ta_decoder_setup(d, pool, false);
		return;
	}
	d->prepared = false;
	d->index = pool->index;
	if (++d->serial == 0)
		d->serial = 1;
	/* begin_frame() waited for this slot's fence: its last frame is done */
	if ((s32)(r->slotSerial[d->index] - d->done) > 0)
		d->done = r->slotSerial[d->index];
	if (r->slotTimed[d->index])
	{
		u64 ts[2];
		if (vkGetQueryPoolResults(vkc.device, r->queries, 2 * (u32)d->index, 2, sizeof(ts), ts, sizeof(u64),
				VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
		{
			const double ms = (double)((ts[1] - ts[0]) & r->mask) * r->period / 1e6;
			d->statGpuMs += ms;
			if (ms > d->statGpuMaxMs)
				d->statGpuMaxMs = ms;
			d->statGpuTimed++;
		}
	}
	r->slotSerial[d->index] = d->serial;
	r->slotTimed[d->index] = false;
	poll_done(d, pool);
	if (d->sortable)
	{
		d->draw.sortedOffset = (VkDeviceSize)d->index * r->sortSlot;
		d->draw.sortedMap = (u32 *)(r->sortedMap + d->draw.sortedOffset);
	}
}

bool vk_ta_decoder_prepare(vk_ta_decoder *d, TA_context *ctx, u32 want, bool *rv)
{
	vk_ta_res *r = d->res;
	d->prepared = false;
	if (ctx->pool < 0 || ctx->rend.isRenderFramebuffer)
	{
		/* (a frame that found the pool full is in memory the GPU cannot read) */
		d->statSkipped++;
		d->statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	d->flags = want;
	d->prepared = ta_gpu_prepare(ctx, &r->layout, r->tables + (size_t)d->index * r->tableWords, r->tableWords, &d->frame, d->flags);
	if (!d->prepared)
	{
		d->statSkipped++;
		d->statWhy[d->frame.why < TA_GPU_WHY_COUNT ? d->frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	*rv = d->frame.rv;
	if (ta_gpu_sorted_indices() != 0)
	{
		const double us = ta_gpu_sorted_ns() / 1e3;
		d->statSorted++;
		d->statSortUs += us;
		if (us > d->statSortMaxUs)
			d->statSortMaxUs = us;
	}
	if (d->frame.shadows != 0)
		d->statShadowed++;
	/* (nothing to draw: nothing to decode) */
	if (!d->frame.rv)
		d->prepared = false;
	return true;
}

static void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags src, VkAccessFlags dst)
{
	VkMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = src;
	barrier.dstAccessMask = dst;
	vkCmdPipelineBarrier(cmd, from, to, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* a kernel, and what it wrote made what the next reads: @groups of it, or
 * as many as the frame's words at @at say (@groups 0, @at not 0) */
static bool dispatch(vk_ta_res *r, VkCommandBuffer cmd, int k, u32 groups, u32 at)
{
	const VkPipeline pipeline = kernel(r, k);
	if (pipeline == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	if (at != 0)
		vkCmdDispatchIndirect(cmd, r->buffers[B_FRM].buffer, (VkDeviceSize)at * 4);
	else if (groups != 0)
		vkCmdDispatch(cmd, groups, 1, 1);
	memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
	return true;
}

/* the kernels, in turn */
static bool kernels(vk_ta_decoder *d, VkCommandBuffer cmd, u32 checks)
{
	vk_ta_res *r = d->res;
	const TaGpuFrame *frame = &d->frame;
	const u32 scanGroups = (frame->verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	if (!dispatch(r, cmd, TAG_K_UNPACK, (frame->verts + frame->modtrig + 63) / 64, 0))
		return false;
	/* (the vertices as unpacked, before TAG_K_PROVOKE moves colours) */
	if (checks != 0 && !dispatch(r, cmd, TAG_K_PREPASS, checks < 65535u ? checks : 65535u, 0))
		return false;
	if (!dispatch(r, cmd, TAG_K_COUNT, scanGroups, 0)
			|| !dispatch(r, cmd, TAG_K_COUNTSUM, 1, 0)
			|| !dispatch(r, cmd, TAG_K_ENDS, scanGroups, 0)
			|| !dispatch(r, cmd, TAG_K_SUMMARY, 0, TAG_O_ARGS64)
			|| !dispatch(r, cmd, TAG_K_GROUPS, 0, TAG_O_ARGSCH)
			|| !dispatch(r, cmd, TAG_K_GROUPSUM, 1, 0)
			|| !dispatch(r, cmd, TAG_K_STATES, 0, TAG_O_ARGSCH)
			|| !dispatch(r, cmd, TAG_K_EMIT, 0, TAG_O_ARGS64)
			|| !dispatch(r, cmd, TAG_K_FINISH, (frame->slots + 63) / 64, 0))
		return false;
	/* (a workgroup for each group named, up to the most a dispatch can have: then each takes several) */
	if (frame->shadows != 0 && !dispatch(r, cmd, TAG_K_SHADOW, frame->shadows < 65535u ? frame->shadows : 65535u, 0))
		return false;
	if (frame->trd != 0 && !dispatch(r, cmd, TAG_K_TRDRAW, 0, TAG_O_ARGS64))
		return false;
	if (screen_height > 480 && !dispatch(r, cmd, TAG_K_BLEED, 0, TAG_O_ARGS1))
		return false;
	if ((d->flags & TA_GPU_PROVOKE) && !dispatch(r, cmd, TAG_K_PROVOKE, 0, TAG_O_ARGS1))
		return false;
	return true;
}

bool vk_ta_decoder_record(vk_ta_decoder *d, TA_context *ctx, vk_cmd_pool *pool)
{
	vk_ta_res *r = d->res;
	VkCommandBufferBeginInfo info;
	VkCommandBuffer cmd;
	VkBuffer frm;
	VkDescriptorSet sets[2];
	u32 checks;

	if (!d->prepared)
		return true;
	d->prepared = false;
	cmd = vk_cmd_pool_allocate(pool);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &info);
	if (r->queries != VK_NULL_HANDLE)
	{
		vkCmdResetQueryPool(cmd, r->queries, 2 * (u32)d->index, 2);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r->queries, 2 * (u32)d->index);
	}
	/* The outputs are one copy: whatever read the last frame's (its draws,
	 * from the next stage on, or its own kernels) is done before they are
	 * written again. (The per-pixel renderer reads the translucent polygon
	 * parameters in its fragment shaders; the drawers copy from the
	 * frame's words.) */
	memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
			| VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
			| VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT
			| VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	frm = r->buffers[B_FRM].buffer;
	/* Two fills that do not overlap: two transfer writes to the same word
	 * with no barrier between them can land in either order */
	vkCmdFillBuffer(cmd, frm, TAG_O_ZMAX * 4, 4, 0x3F800000u);	/* fZ_max starts at 1 */
	vkCmdFillBuffer(cmd, frm, 4, (TAG_O_WORDS - 1) * 4, 0);
	if (d->frame.slots != 0)
		vkCmdFillBuffer(cmd, r->buffers[B_WORK].buffer, (VkDeviceSize)r->layout.words[9] * 4, (VkDeviceSize)d->frame.slots * 4, 0);
	/* the frame's translucent draw records: counts and room */
	if (d->frame.trd != 0)
		vkCmdFillBuffer(cmd, r->buffers[B_TRD].buffer, 0, (VkDeviceSize)d->frame.trd * TAG_C_WORDS * 4, 0);
	memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: written now, before the kernel that reads
	 * them is (the host's writes before the submit are the GPU's) */
	checks = d->sortable && d->draw.sortedMap != NULL && ta_gpu_sorted_indices() != 0 ? ta_gpu_prepass_checks() : 0;
	if (d->sortable && d->draw.sortedMap != NULL && ta_gpu_sorted_indices() != 0)
		ta_gpu_sorted_write(d->draw.sortedMap);

	sets[0] = r->sliceSets[ctx->pool];
	sets[1] = r->tableSets[d->index];
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipelineLayout, 0, 2, sets, 0, NULL);
	if (!kernels(d, cmd, checks))
		return false;

	/* for whatever draws from them (and the drawers' copy of 1/fZ_max out of the frame's words) */
	memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
			| VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
			| VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT
			| VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
	if (r->queries != VK_NULL_HANDLE)
	{
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r->queries, 2 * (u32)d->index + 1);
		r->slotTimed[d->index] = true;
	}
	vkEndCommandBuffer(cmd);
	/* the context's slice is read until this frame is done */
	ctx->gpu_serial = d->serial;
	if (++d->statFrames % 600 == 0)
		say(d, " so far");
	return true;
}
