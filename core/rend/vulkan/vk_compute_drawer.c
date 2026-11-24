#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/* The compute renderer on Vulkan: see vk_compute_drawer.h */
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "vk_compute_drawer.h"
#include "vk_drawer_base.h"
#include "writeout.h"
#include "pso.h"
#include "rend/ssaa_pattern.h"
#include "rend/upscaler.h"
#include "rend/ta_gpu.h"
#include "hw/pvr/pvr_mem.h"

/* The kernels, by PVRC_K_*, as SPIR-V made from rend/tile/pvr_compute.h
 * when the source was: tools/rend/kernels_spv.sh, which checks they still
 * are its. Nothing is compiled when the core runs. */
#include "compute_spv.inc"
/* (a kernel for each PVRC_K_*) */
typedef char vk_compute_kernels_check[sizeof(compute_spv) / sizeof(compute_spv[0]) == PVRC_KERNELS + 1 ? 1 : -1];

/* (for the raster renderers' write-out: vk_writeout.c) */
const u32 *vk_compute_kernel(u32 kernel, size_t *size)
{
	*size = compute_spv_size[kernel];
	return compute_spv[kernel];
}

/* (the walk's unsorted variant's pipeline, after the kernels') */
#define WALK_UNSORTED (PVRC_KERNELS + 1)
#define KINDS (PVRC_KERNELS + 2)

/* The bin pool's entries: a run for each tile a primitive touches, for a
 * frame's every primitive (4 MiB) */
#define POOL_ENTRIES (2u << 20)

/* The bindings of the kernels' one set (B_SUMS: the HDR output's channel sums, pvr_compute.h's PVRC_SUMS) */
enum { B_FRAME, B_REC, B_BIN, B_OUT, B_LAST, B_PAL, B_TEX, B_VTX, B_IDX, B_MOD, B_CMD, B_TRD, B_DFRM, B_SUMS, B_DEPTH, B_COUNT };
/* (the buffers a frame of the GPU's TA decode is read from: B_VTX to B_DFRM) */
#define GPU_BINDINGS (B_SUMS - B_VTX)

/* what PVRC_K_GPUSRC reads of the decode's buffers */
typedef char vk_compute_decode_check[PVRC_TAG_C_WORDS == TAG_C_WORDS && PVRC_TAG_C_COUNT == TAG_C_COUNT && PVRC_TAG_C_FIRST == TAG_C_FIRST
		&& PVRC_TAG_O_OVERRUN == TAG_O_OVERRUN && PVRC_TAG_O_ZSCALE == TAG_O_ZSCALE ? 1 : -1];

/* ---- the frame's words ---- */

void vk_compute_frame_init(vk_compute_frame *f)
{
	transform_matrix_init(&f->matrices, false);
}

const xform *vk_compute_frame_begin(vk_compute_frame *f)
{
	transform_matrix_calc(&f->matrices, &pvrrc);
	/* the screen's frame under the temporal upscaler's jitter (rend/upscaler.h), as the raster renderers draw it */
	if (!pvrrc.isRTT && !pvrrc.gpu && upscaler_active())
	{
		float jx, jy;
		upscaler_jitter(&jx, &jy);
		transform_matrix_jitter(&f->matrices, jx, jy);
	}
	return &f->matrices.viewport;
}

void vk_compute_frame_base_scissor(vk_compute_frame *f, int *x, int *y, int *width, int *height)
{
	VkRect2D rect;
	vk_drawer_base_scissor(&f->matrices, &rect);
	*x = rect.offset.x;
	*y = rect.offset.y;
	*width = (int)rect.extent.width;
	*height = (int)rect.extent.height;
}

void vk_compute_frame_provoking_vertices(vk_compute_frame *f)
{
	(void)f;
	vk_drawer_provoking_vertices();
}

/* ---- the Vulkan side ---- */

/* The textures a frame's polygons have, each given a slot: a table of a
 * fixed size, emptied by a new generation each frame */
#define SLOT_TABLE_SIZE (2 * PVRC_TEX_SLOTS)

typedef struct slot_entry
{
	u64 texid;
	u32 tsp;
	u32 gen;
	u32 slot;
} slot_entry;

typedef struct slot_table
{
	slot_entry entries[SLOT_TABLE_SIZE];
	u64 slotTex[PVRC_TEX_SLOTS];
	u32 slotTsp[PVRC_TEX_SLOTS];
	u32 gen;
	u32 used;
} slot_table;

static void slots_reset(slot_table *t)
{
	if (++t->gen == 0)
	{
		memset(t->entries, 0, sizeof(t->entries));
		t->gen = 1;
	}
	t->used = 0;
}

static u32 slots_slot(slot_table *t, u64 texid, u32 tsp)
{
	const u32 mask = tsp & VK_SAMPLER_TSP_MASK;
	const u32 h = (u32)(texid >> 4) * 2654435761u ^ mask * 40503u;
	u32 i;
	for (i = 0; i < SLOT_TABLE_SIZE; i++)
	{
		slot_entry *e = &t->entries[(h + i) & (SLOT_TABLE_SIZE - 1)];
		if (e->gen != t->gen)
		{
			if (t->used == PVRC_TEX_SLOTS)
				return PVRC_NONE;
			e->gen = t->gen;
			e->texid = texid;
			e->tsp = tsp;
			e->slot = t->used;
			t->slotTex[t->used] = texid;
			t->slotTsp[t->used] = tsp;
			return t->used++;
		}
		if (e->texid == texid && (e->tsp & VK_SAMPLER_TSP_MASK) == mask)
			return e->slot;
	}
	return PVRC_NONE;
}

/* What a set of a slot of the pool has: its texture slots, and the buffers
 * its B_VTX to B_DFRM are (the ring's slot, or the decode's) */
typedef struct set_state
{
	VkImageView views[PVRC_TEX_SLOTS];
	VkSampler samplers[PVRC_TEX_SLOTS];
	u32 used;
	VkBuffer buffers[GPU_BINDINGS];
} set_state;

/* (its kernels' pipelines: asked of the pipeline thread by init(), pso.h;
 * the first frame takes them, or makes them there and fails there as init()
 * did when the device cannot). One allocation, with the slots' arrays after
 * it. */
typedef struct vk_compute_res
{
	vk_pso_builder builder;
	/* what the kernels are made of */
	VkShaderModule modules[KINDS];
	u32 samples;
	bool depth;
	/* (kernels: the screen's, and the walk's unsorted variant last:
	 * WALK_UNSORTED; then with SSAA, the kernels for renders to a texture,
	 * at one sample a pixel, and with the HDR output, the walks for them,
	 * which store no sums: the screen's do - empty where they are the
	 * screen's) */
	PsoSlot kernels[2 * KINDS];

	int slots;
	vk_buffer ring;			/* a piece for each slot of the pool */
	u32 ringWords;
	u32 *ringMap;
	vk_buffer records;
	vk_buffer bins;
	vk_buffer stats;		/* the counts, a piece a slot, read when it comes round */
	u32 *statsMap;
	bool *statsWritten;
	u32 binWords;
	u32 runs, chunks, tileLog2;
	u32 keep;
	bool binsZero;
	VkDescriptorSetLayout setLayout;
	VkPipelineLayout pipelineLayout;
	/* With the HDR output, each pixel's channel sums, which the screen's
	 * walk stores (hdr_encode_vk.h: of HDR_VK_SUMS_SAMPLES); otherwise an
	 * image of one pixel, for the binding */
	vk_attachment sums;
	/* For the upscaler, each pixel's depth, an image a slot (pvr_compute.h's
	 * PVRC_DEPTH_ON); otherwise one image of one pixel, for the binding */
	vk_attachment *depths;
	bool sumsOn;
	bool sumsLayout;	/* in the general layout */
	bool sumsStored;	/* by the frame just recorded (and nothing over them since) */
	VkDescriptorPool descriptorPool;
	VkDescriptorSet *sets;
	set_state *setStates;
	slot_table slotTable;
	VkImageView dummyView;
	VkDescriptorImageInfo imageInfos[PVRC_TEX_SLOTS + 8];
	VkWriteDescriptorSet writes[PVRC_TEX_SLOTS + 8 + GPU_BINDINGS];
	VkDescriptorBufferInfo bufferInfos[GPU_BINDINGS];
	u32 nImages, nWrites, nBuffers;
	VkSampler nearest;
	VkSampler linear;	/* (a picture of another size to start from) */
	bool lastScaled;
	bool gpu;		/* this frame's is the decode's */
	bool rtt;		/* ...a render to a texture */
	/* this frame's, between prepare and record */
	u32 words, prims;
	u64 skipped, dropped, hiz, clipped;
	u32 peels;
} vk_compute_res;

/* @key: the set's (0 the screen's, 1 for renders to a texture) times
 * KINDS, and the kernel */
static VkPipeline make(const vk_compute_res *r, u64 key, VkPipelineCache cache)
{
	const int set = (int)(key / KINDS), k = (int)(key % KINDS);
	const u32 n = set == 0 ? r->samples : 1;
	u32 spec[7];
	VkSpecializationMapEntry entries[7];
	VkSpecializationInfo specInfo;
	VkComputePipelineCreateInfo info;
	unsigned i;

	spec[0] = n;
	spec[1] = SSAA_LOG2(n);
	spec[2] = k == WALK_UNSORTED ? 1u : r->keep;
	spec[3] = 0;
	spec[4] = k == WALK_UNSORTED ? PVRC_V_UNSORTED : k == PVRC_K_WALK ? PVRC_V_SORTED : PVRC_V_ALL;
	spec[5] = set == 0 && r->sumsOn ? 1u : 0u;
	spec[6] = set == 0 && r->depth ? 1u : 0u;
	for (i = 0; i < 7; i++)
	{
		entries[i].constantID = i;
		entries[i].offset = i * 4;
		entries[i].size = 4;
	}
	specInfo.mapEntryCount = 7;
	specInfo.pMapEntries = entries;
	specInfo.dataSize = sizeof(spec);
	specInfo.pData = spec;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = r->modules[k];
	info.stage.pName = "main";
	info.stage.pSpecializationInfo = &specInfo;
	info.layout = r->pipelineLayout;
	return vk_pso_create_compute(vkc.device, cache, &info);
}

/* the pipeline thread's */
static VkPipeline build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	return make((const vk_compute_res *)self, key, cache);
}

/* (one the pipeline thread could not make: made here, or none, as init() would have failed) */
static VkPipeline kernel(vk_compute_res *r, int set, int k)
{
	PsoSlot *slot = &r->kernels[set * KINDS + k];
	const VkPipeline made = vk_pso_get(slot);
	if (made != VK_NULL_HANDLE)
		return made;
	return PsoOwnMade(slot, make(r, slot->key, vkc.pipelineCache));
}

static bool has(const vk_compute_res *r, int set, int k)
{
	return retro_atomic_load_relaxed_int(&((vk_compute_res *)r)->kernels[set * KINDS + k].state) != PSO_EMPTY;
}

static void res_free(vk_compute_res *r)
{
	int i;
	/* (the pipeline thread done with what it has of them before the modules go) */
	PsoOwnDrop(&r->builder, r->kernels, 2 * KINDS, vkc.device);
	if (r->linear != VK_NULL_HANDLE)
		vkDestroySampler(vkc.device, r->linear, NULL);
	if (r->nearest != VK_NULL_HANDLE)
		vkDestroySampler(vkc.device, r->nearest, NULL);
	/* (the sets with it) */
	if (r->descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(vkc.device, r->descriptorPool, NULL);
	for (i = 0; i < r->slots; i++)
		vk_attachment_free(&r->depths[i]);
	vk_attachment_free(&r->sums);
	if (r->pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vkc.device, r->pipelineLayout, NULL);
	if (r->setLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, r->setLayout, NULL);
	vk_buffer_free(&r->stats);
	vk_buffer_free(&r->bins);
	vk_buffer_free(&r->records);
	vk_buffer_free(&r->ring);
	for (i = 0; i < KINDS; i++)
		if (r->modules[i] != VK_NULL_HANDLE)
			vkDestroyShaderModule(vkc.device, r->modules[i], NULL);
	free(r);
}

void vk_compute_drawer_construct(vk_compute_drawer *d)
{
	memset(d, 0, sizeof(*d));
	d->samples = 1;
	d->timer.what = "";
	vk_compute_frame_init(&d->frame);
}

const char *vk_compute_drawer_device_lacks(void)
{
	VkPhysicalDeviceProperties props;
	const VkPhysicalDeviceLimits *limits = &props.limits;
	vkGetPhysicalDeviceProperties(vkc.gpu, &props);
	if (!vkc.shaderInt64)
		return "no 64-bit integers in shaders";
	if (!vkc.sampledImageArrayIndexing)
		return "no indexing of an array of textures";
	if (!vkc.nonUniformTextureIndexing && !vkc.subgroupBallotCompute)
		return "no non-uniform indexing of an array of textures, nor subgroup ballot";
	if (limits->maxComputeWorkGroupInvocations < PVRC_WG || limits->maxComputeWorkGroupSize[0] < PVRC_WG)
		return "workgroups too small";
	if (limits->maxPerStageDescriptorSamplers < PVRC_TEX_SLOTS + 2 || limits->maxPerStageDescriptorSampledImages < PVRC_TEX_SLOTS + 2
			|| limits->maxDescriptorSetSamplers < PVRC_TEX_SLOTS + 2 || limits->maxDescriptorSetSampledImages < PVRC_TEX_SLOTS + 2)
		return "too few texture bindings";
	if (limits->maxComputeSharedMemorySize < PVRC_SORT_MAX * 4)
		return "too little shared memory";
	if (limits->maxPerStageDescriptorStorageBuffers < 3 + GPU_BINDINGS || limits->maxDescriptorSetStorageBuffers < 3 + GPU_BINDINGS)
		return "too few storage buffers";
	return NULL;
}

static u32 tiles_of(u32 width, u32 height, u32 log2)
{
	return ((width + (1u << log2) - 1) >> log2) * ((height + (1u << log2) - 1) >> log2);
}

static void buffer_write(VkWriteDescriptorSet *w, VkDescriptorBufferInfo *info, VkDescriptorSet set, u32 binding,
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

static void image_write(VkWriteDescriptorSet *w, VkDescriptorImageInfo *info, VkDescriptorSet set, u32 binding, u32 element,
		VkDescriptorType type, VkSampler sampler, VkImageView view, VkImageLayout layout)
{
	info->sampler = sampler;
	info->imageView = view;
	info->imageLayout = layout;
	memset(w, 0, sizeof(*w));
	w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w->dstSet = set;
	w->dstBinding = binding;
	w->dstArrayElement = element;
	w->descriptorCount = 1;
	w->descriptorType = type;
	w->pImageInfo = info;
}

/* the bins, and (@withSums, @withDepths) the sums and depths, for a
 * picture of @width x @height: false if one could not be made (what was
 * made then freed) */
static bool make_sized(vk_compute_res *r, u32 width, u32 height, u32 binWords, vk_buffer *bins, vk_attachment *sums,
		vk_attachment *depths, bool withSums, bool withDepths)
{
	int i;
	memset(sums, 0, sizeof(*sums));
	if (vk_buffer_init(bins, (VkDeviceSize)binWords * 4,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != VK_SUCCESS)
		return false;
	if (withSums && !vk_attachment_init(sums, r->sumsOn ? width : 1, r->sumsOn ? height : 1, VK_FORMAT_R16G16B16A16_UINT,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, VK_SAMPLE_COUNT_1_BIT))
	{
		vk_buffer_free(bins);
		return false;
	}
	for (i = 0; withDepths && i < (r->depth ? r->slots : 1); i++)
	{
		memset(&depths[i], 0, sizeof(depths[i]));
		if (!vk_attachment_init(&depths[i], r->depth ? width : 1, r->depth ? height : 1, VK_FORMAT_R32_SFLOAT,
				VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false,
				VK_SAMPLE_COUNT_1_BIT))
		{
			while (i-- > 0)
				vk_attachment_free(&depths[i]);
			vk_attachment_free(sums);
			vk_buffer_free(bins);
			return false;
		}
	}
	return true;
}

/* the sets' B_BIN, B_SUMS and B_DEPTH: what the bins and images are now */
static void write_sized(vk_compute_res *r)
{
	VkDescriptorBufferInfo bins;
	VkDescriptorImageInfo sums, depth;
	VkWriteDescriptorSet writes[3];
	int s;
	for (s = 0; s < r->slots; s++)
	{
		buffer_write(&writes[0], &bins, r->sets[s], B_BIN, r->bins.buffer, 0, VK_WHOLE_SIZE);
		image_write(&writes[1], &sums, r->sets[s], B_SUMS, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, r->sums.view,
				VK_IMAGE_LAYOUT_GENERAL);
		image_write(&writes[2], &depth, r->sets[s], B_DEPTH, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
				r->depths[r->depth ? s : 0].view, VK_IMAGE_LAYOUT_GENERAL);
		vkUpdateDescriptorSets(vkc.device, 3, writes, 0, NULL);
	}
}

/* Another size with the same kernels (samples, depth, channel sums): the
 * bins, sums and depths made again for it, the kernels kept. False when
 * it cannot (init then makes everything) */
static bool resize(vk_compute_drawer *d, u32 width, u32 height)
{
	vk_compute_res *r = d->res;
	const u32 tileLog2 = compute_frame_tile_log2(width);
	const u32 tiles = tiles_of(width, height, tileLog2);
	const u32 most_tiles = tiles > compute_frame_max_rtt_tiles() ? tiles : compute_frame_max_rtt_tiles();
	const u32 runs = r->chunks * most_tiles;
	const u32 binWords = pvrc_bin_words(runs, POOL_ENTRIES);
	const u64 most = vkc.maxStorageBufferRange < vkc.maxMemoryAllocationSize ? vkc.maxStorageBufferRange : vkc.maxMemoryAllocationSize;
	vk_buffer bins;
	vk_attachment sums;
	vk_attachment *depths;
	int i;

	if ((u64)binWords * 4 > most)
		return false;
	/* (the bins' growth, against what the budget has left) */
	if (binWords > r->binWords && ((u64)(binWords - r->binWords) * 4 >> 10) > vram_budget_available_kib(&vkc.vramBudget))
		return false;
	/* (made with the renderer, as it changes size: never while a game runs) */
	depths = (vk_attachment *)calloc((size_t)r->slots, sizeof(*depths));
	if (depths == NULL)
		return false;
	if (!make_sized(r, width, height, binWords, &bins, &sums, depths, r->sumsOn, r->depth))
	{
		free(depths);
		return false;
	}
	/* (the renderer waited for the frames that used the old ones) */
	vk_buffer_free(&r->bins);
	r->bins = bins;
	r->binsZero = false;
	if (r->sumsOn)
	{
		vk_attachment_free(&r->sums);
		r->sums = sums;
		r->sumsLayout = false;
		r->sumsStored = false;
	}
	if (r->depth)
		for (i = 0; i < r->slots; i++)
		{
			vk_attachment_free(&r->depths[i]);
			r->depths[i] = depths[i];
		}
	free(depths);
	r->tileLog2 = tileLog2;
	r->runs = runs;
	r->binWords = binWords;
	write_sized(r);
	d->extent.width = width;
	d->extent.height = height;
	NOTICE_LOG(RENDERER, "Vulkan compute renderer: %u x %u, tiles of %u, its kernels kept\n", width, height, 1u << r->tileLog2);
	return true;
}

static bool fail(vk_compute_drawer *d, vk_compute_res *r, const char *why)
{
	INFO_LOG(RENDERER, "Vulkan compute renderer: not on this device (%s); the raster renderer draws\n", why);
	d->failed = true;
	if (r != NULL)
		res_free(r);
	return false;
}

static VkSampler make_sampler(VkFilter filter)
{
	VkSamplerCreateInfo info;
	VkSampler sampler = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = filter;
	info.minFilter = filter;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	vkCreateSampler(vkc.device, &info, NULL, &sampler);
	return sampler;
}

/* the layouts, the kernels asked for, the sets and samplers */
static bool make_kernels(vk_compute_drawer *d, vk_compute_res *r)
{
	VkDescriptorSetLayoutBinding bindings[B_COUNT];
	VkDescriptorSetLayoutCreateInfo setInfo;
	VkPipelineLayoutCreateInfo layoutInfo;
	VkDescriptorPoolSize sizes[3];
	VkDescriptorPoolCreateInfo poolInfo;
	VkDescriptorSetAllocateInfo allocInfo;
	PsoSlot *asked[2 * KINDS];
	u32 n = 0;
	int k, set, s, b;

	memset(bindings, 0, sizeof(bindings));
	for (b = 0; b < B_COUNT; b++)
	{
		bindings[b].binding = (u32)b;
		bindings[b].descriptorCount = 1;
		bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}
	bindings[B_OUT].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[B_LAST].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[B_PAL].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[B_TEX].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[B_TEX].descriptorCount = PVRC_TEX_SLOTS;
	bindings[B_SUMS].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[B_DEPTH].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = B_COUNT;
	setInfo.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &r->setLayout) != VK_SUCCESS)
		return false;
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &r->setLayout;
	if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &r->pipelineLayout) != VK_SUCCESS)
		return false;

	/* every kernel, asked for now (of the pipeline thread), for this many
	 * samples, and for one when that is not one (renders to a texture):
	 * none is made while a game runs. The walk in its two variants
	 * (pvr_compute.h: PVRC_V_*), the one for the tiles without sorted
	 * translucent fragments keeping none. (With the HDR output, the
	 * screen's walks store the channel sums; at one sample a pixel, renders
	 * to a texture have walks of their own, which do not.) */
	for (k = 1; k <= WALK_UNSORTED; k++)
	{
		const bool walk = k == PVRC_K_WALK || k == WALK_UNSORTED;
		/* (the walk's textures taken in turns where the device cannot index them non-uniformly) */
		const bool waterfall = walk && !vkc.nonUniformTextureIndexing;
		VkShaderModuleCreateInfo moduleInfo;
		memset(&moduleInfo, 0, sizeof(moduleInfo));
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = waterfall ? sizeof(compute_spv_walk_waterfall) : compute_spv_size[walk ? PVRC_K_WALK : k];
		moduleInfo.pCode = waterfall ? compute_spv_walk_waterfall : compute_spv[walk ? PVRC_K_WALK : k];
		if (vkCreateShaderModule(vkc.device, &moduleInfo, NULL, &r->modules[k]) != VK_SUCCESS)
			return false;
	}
	for (set = 0; set < (d->samples > 1 || r->sumsOn || d->depth ? 2 : 1); set++)
		for (k = 1; k <= WALK_UNSORTED; k++)
		{
			const bool walk = k == PVRC_K_WALK || k == WALK_UNSORTED;
			if (set == 1 && d->samples <= 1 && !walk)
				continue;
			asked[n++] = &r->kernels[set * KINDS + k];
		}
	vk_pso_worker_build_soon(asked, n, vkc.pipelineCache);

	sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[0].descriptorCount = (3u + GPU_BINDINGS) * (u32)r->slots;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = 3u * (u32)r->slots;
	sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[2].descriptorCount = (PVRC_TEX_SLOTS + 2u) * (u32)r->slots;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = (u32)r->slots;
	poolInfo.poolSizeCount = 3;
	poolInfo.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(vkc.device, &poolInfo, NULL, &r->descriptorPool) != VK_SUCCESS)
		return false;
	/* (a set at a time: their count is the pool's) */
	memset(&allocInfo, 0, sizeof(allocInfo));
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = r->descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &r->setLayout;
	for (s = 0; s < r->slots; s++)
	{
		if (vkAllocateDescriptorSets(vkc.device, &allocInfo, &r->sets[s]) != VK_SUCCESS)
			return false;
		r->setStates[s].used = PVRC_TEX_SLOTS;	/* (all of them to be written the first time) */
	}
	r->nearest = make_sampler(VK_FILTER_NEAREST);
	r->linear = make_sampler(VK_FILTER_LINEAR);
	if (r->nearest == VK_NULL_HANDLE || r->linear == VK_NULL_HANDLE)
		return false;

	for (s = 0; s < r->slots; s++)
	{
		VkDescriptorBufferInfo buffers[3 + GPU_BINDINGS];
		VkWriteDescriptorSet writes[3 + GPU_BINDINGS];
		const VkDeviceSize piece = (VkDeviceSize)r->ringWords * 4;
		buffer_write(&writes[0], &buffers[0], r->sets[s], B_FRAME, r->ring.buffer, (VkDeviceSize)s * piece, piece);
		buffer_write(&writes[1], &buffers[1], r->sets[s], B_REC, r->records.buffer, 0, VK_WHOLE_SIZE);
		buffer_write(&writes[2], &buffers[2], r->sets[s], B_BIN, r->bins.buffer, 0, VK_WHOLE_SIZE);
		/* (a frame of the parse's reads its vertices, indices and volumes from its own words; prepare changes them for the decode's) */
		for (b = 0; b < GPU_BINDINGS; b++)
		{
			buffer_write(&writes[3 + b], &buffers[3 + b], r->sets[s], (u32)(B_VTX + b), r->ring.buffer, (VkDeviceSize)s * piece, piece);
			r->setStates[s].buffers[b] = r->ring.buffer;
		}
		vkUpdateDescriptorSets(vkc.device, 3 + GPU_BINDINGS, writes, 0, NULL);
	}
	write_sized(r);
	return true;
}

bool vk_compute_drawer_init(vk_compute_drawer *d, u32 width, u32 height, u32 samples, vk_cmd_pool *pool, vk_samplers *samplers,
		bool depth)
{
	const u32 n = samples > 4 ? 8 : samples > 2 ? 4 : samples > 1 ? 2 : 1;
	VkPhysicalDeviceProperties props;
	const char *lacks;
	vk_compute_res *r;
	u32 tiles, mostTiles, slots;
	u64 most, deviceBytes;

	/* (a change of size alone: nothing for the pipeline thread to make,
	 * and nothing to wait for before the next frame) */
	if (d->res != NULL && !d->failed && d->res->samples == n && d->res->depth == depth && pool == d->pool
			&& samplers == d->samplers && d->res->sumsOn == vkc_hdr_sums_ready()
			&& (int)pool->slotCount == d->res->slots && resize(d, width, height))
		return true;
	vk_compute_drawer_term(d);
	if (d->failed)
		return false;
	vkGetPhysicalDeviceProperties(vkc.gpu, &props);
	d->extent.width = width;
	d->extent.height = height;
	d->samples = n;
	d->pool = pool;
	d->samplers = samplers;
	d->depth = depth;

	lacks = vk_compute_drawer_device_lacks();
	if (lacks != NULL)
		return fail(d, NULL, lacks);
	slots = pool->slotCount;
	/* (one allocation, with the slots' arrays after it: made with the renderer, never while a game runs) */
	r = (vk_compute_res *)calloc(1, sizeof(*r) + slots * (sizeof(VkDescriptorSet) + sizeof(set_state) + sizeof(vk_attachment)
			+ sizeof(bool)));
	if (r == NULL)
		return fail(d, NULL, "no memory");
	r->builder.build = build;
	retro_atomic_int_init(&r->builder.generation, 0);
	PsoOwnInit(&r->builder, r->kernels, 2 * KINDS);
	r->slots = (int)slots;
	r->sets = (VkDescriptorSet *)(r + 1);
	r->setStates = (set_state *)(r->sets + slots);
	r->depths = (vk_attachment *)(r->setStates + slots);
	r->statsWritten = (bool *)(r->depths + slots);
	r->samples = n;
	r->depth = depth;
	r->keep = pvrc_fragments_kept(props.limits.maxComputeSharedMemorySize);
	r->tileLog2 = compute_frame_tile_log2(width);
	tiles = tiles_of(width, height, r->tileLog2);
	r->chunks = (compute_frame_max_prims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	/* (room for the screen's tiles, and a texture rendered to's: a few words each) */
	mostTiles = tiles > compute_frame_max_rtt_tiles() ? tiles : compute_frame_max_rtt_tiles();
	r->runs = r->chunks * mostTiles;
	r->binWords = pvrc_bin_words(r->runs, POOL_ENTRIES);
	r->ringWords = (compute_frame_max_words() + 63) & ~63u;
	most = vkc.maxStorageBufferRange < vkc.maxMemoryAllocationSize ? vkc.maxStorageBufferRange : vkc.maxMemoryAllocationSize;
	if ((u64)r->ringWords * 4 > most || (u64)compute_frame_max_prims() * PVRC_REC_WORDS * 4 > most || (u64)r->binWords * 4 > most)
		return fail(d, r, "buffers larger than the device has");
	deviceBytes = (u64)compute_frame_max_prims() * PVRC_REC_WORDS * 4 + (u64)r->binWords * 4;
	if ((deviceBytes >> 10) > vram_budget_available_kib(&vkc.vramBudget))
		return fail(d, r, "not enough video memory");
	/* (the motion vector pass draws the frame's vertices and indices from the ring) */
	if (vk_buffer_init(&r->ring, (VkDeviceSize)r->ringWords * 4 * slots,
			depth ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
				: VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return fail(d, r, "the ring could not be made");
	r->ringMap = (u32 *)vk_buffer_map(&r->ring);
	if (vk_buffer_init(&r->records, (VkDeviceSize)compute_frame_max_prims() * PVRC_REC_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != VK_SUCCESS
			|| vk_buffer_init(&r->stats, (VkDeviceSize)PVRC_STATS * 4 * slots, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return fail(d, r, "the buffers could not be made");
	r->statsMap = (u32 *)vk_buffer_map(&r->stats);

	/* (with the HDR output, the walk's channel sums: what was resolved,
	 * SSAA's samples and the X scaler's pairs, has more than 8 bits) */
	r->sumsOn = vkc_hdr_sums_ready();
	if (!make_sized(r, width, height, r->binWords, &r->bins, &r->sums, r->depths, true, true))
		return fail(d, r, "the bins or images could not be made");
	if (!make_kernels(d, r))
		return fail(d, r, "its kernels could not be set up");

	vk_gpu_timer_init(&d->timer, (int)slots, "compute renderer");
	d->res = r;
	d->ready = true;
	NOTICE_LOG(RENDERER, "Vulkan compute renderer: %u x %u, %u sample%s a pixel, tiles of %u, %u translucent fragments a sample at a time; %u KiB of video memory, %u KiB of host memory\n",
			width, height, d->samples, d->samples > 1 ? "s" : "", 1u << r->tileLog2, r->keep,
			(u32)(deviceBytes >> 10), (u32)(((u64)r->ringWords * 4 * slots) >> 10));
	return true;
}

void vk_compute_drawer_term(vk_compute_drawer *d)
{
	if (d->res != NULL && d->statFrames + d->statFallback != 0)
	{
		const vk_compute_res *r = d->res;
		NOTICE_LOG(RENDERER, "Vulkan compute renderer: %llu frames drawn (%llu of them the GPU's TA decode's, %llu renders to a texture), %llu left to the raster renderer; %llu runs walked over their chunk, %llu triangles at no depth left out, %llu with a vertex behind the eye clipped, at most %u rounds of translucent fragments, %llu primitives passed over by the tiles' depths\n",
				(unsigned long long)d->statFrames, (unsigned long long)d->statGpuFrames, (unsigned long long)d->statRttFrames,
				(unsigned long long)d->statFallback, (unsigned long long)r->skipped, (unsigned long long)r->dropped,
				(unsigned long long)r->clipped, r->peels, (unsigned long long)r->hiz);
		NOTICE_LOG(RENDERER, "Vulkan compute renderer: fallbacks: %llu renders to a texture it cannot write, %llu with the VMUs or crosshairs drawn over, %llu starting from a picture it cannot take, %llu with more render passes than it takes, %llu with more textures than its slots, %llu too large, %llu the decode's without its buffers\n",
				(unsigned long long)d->statWhy[CF_WHY_RTT], (unsigned long long)d->statWhy[CF_WHY_OSD],
				(unsigned long long)d->statWhy[CF_WHY_PICTURE], (unsigned long long)d->statWhy[CF_WHY_PASSES],
				(unsigned long long)d->statWhy[CF_WHY_TEXTURES], (unsigned long long)d->statWhy[CF_WHY_ROOM],
				(unsigned long long)d->statWhy[CF_WHY_DECODE]);
	}
	vk_gpu_timer_term(&d->timer);
	if (d->res != NULL)
		res_free(d->res);
	d->res = NULL;
	d->ready = false;
	d->statFrames = d->statFallback = d->statGpuFrames = d->statRttFrames = 0;
	memset(d->statWhy, 0, sizeof(d->statWhy));
}

u32 vk_compute_drawer_depth_views(const vk_compute_drawer *d, VkImageView *views, u32 most)
{
	u32 n = 0;
	int i;
	if (d->res != NULL && d->depth)
		for (i = 0; i < d->res->slots && n < most; i++)
			views[n++] = d->res->depths[i].view;
	return n;
}

VkImage vk_compute_drawer_depth_image(const vk_compute_drawer *d)
{
	return d->res != NULL && d->depth ? d->res->depths[d->recordedSlot].image : VK_NULL_HANDLE;
}

VkImageView vk_compute_drawer_depth_view(const vk_compute_drawer *d)
{
	return d->res != NULL && d->depth ? d->res->depths[d->recordedSlot].view : VK_NULL_HANDLE;
}

int vk_compute_drawer_slot(const vk_compute_drawer *d)
{
	return d->pool != NULL ? d->pool->index : 0;
}

VkImageView vk_compute_drawer_sums(const vk_compute_drawer *d)
{
	return d->res != NULL && d->res->sumsStored ? d->res->sums.view : VK_NULL_HANDLE;
}

bool vk_compute_drawer_can_draw(const vk_compute_drawer *d)
{
	return d->ready && !pvrrc.isRenderFramebuffer && !(pvrrc.gpu && d->gpuDraw == NULL);
}

/* compute_frame_pack()'s renderer: the drawer's frame, and its slots */
typedef struct bound_frame
{
	vk_compute_frame *frame;
	slot_table *slots;
} bound_frame;

static u32 bound_slot(void *self, u64 texid, u32 tsp)
{
	return slots_slot(((bound_frame *)self)->slots, texid, tsp);
}

static const xform *bound_begin(void *self)
{
	return vk_compute_frame_begin(((bound_frame *)self)->frame);
}

static void bound_base_scissor(void *self, int *x, int *y, int *width, int *height)
{
	vk_compute_frame_base_scissor(((bound_frame *)self)->frame, x, y, width, height);
}

static void bound_provoking_vertices(void *self)
{
	vk_compute_frame_provoking_vertices(((bound_frame *)self)->frame);
}

bool vk_compute_drawer_prepare(vk_compute_drawer *d, bool last, bool scaled, const vk_texture *paletteTexture,
		u32 rttWidth, u32 rttHeight)
{
	vk_compute_res *r = d->res;
	const int slot = d->pool->index;
	compute_frame_layout layout;
	compute_frame_renderer renderer;
	bound_frame bound;
	u32 tiles, why = CF_WHY_NONE;
	u32 *words;

	/* the counts of the frame that had this slot before, done by now */
	if (r->statsWritten[slot])
	{
		const u32 *st = r->statsMap + slot * PVRC_STATS;
		r->skipped += st[PVRC_ST_SKIPPED];
		r->dropped += st[PVRC_ST_DROPPED];
		r->clipped += st[PVRC_ST_CLIPPED];
		r->hiz += st[PVRC_ST_HIZ];
		if (st[PVRC_ST_PEELS] > r->peels)
			r->peels = st[PVRC_ST_PEELS];
	}

	memset(&layout, 0, sizeof(layout));
	r->rtt = pvrrc.isRTT;
	layout.width = r->rtt ? rttWidth : d->extent.width;
	layout.height = r->rtt ? rttHeight : d->extent.height;
	layout.tileLog2 = r->rtt ? compute_frame_tile_log2(layout.width) : r->tileLog2;
	layout.pool = POOL_ENTRIES;
	layout.runs = r->runs;
	layout.chunks = r->chunks;
	layout.lastPicture = last && !r->rtt;
	layout.lastScaled = layout.lastPicture && scaled;
	layout.rttScale = r->rtt ? compute_frame_rtt_scale() : 0;
	r->lastScaled = layout.lastScaled;
	/* (a texture rendered to larger than the bins are made for: none the PowerVR's registers ask for) */
	tiles = tiles_of(layout.width, layout.height, layout.tileLog2);
	if (layout.width == 0 || layout.height == 0 || (u64)tiles * r->chunks > r->runs)
	{
		vk_compute_drawer_fallback(d, CF_WHY_ROOM);
		return false;
	}
	slots_reset(&r->slotTable);
	words = r->ringMap + (size_t)slot * r->ringWords;
	bound.frame = &d->frame;
	bound.slots = &r->slotTable;
	renderer.self = &bound;
	renderer.slot = bound_slot;
	renderer.begin = bound_begin;
	renderer.base_scissor = bound_base_scissor;
	renderer.provoking_vertices = bound_provoking_vertices;
	r->words = compute_frame_pack(&layout, &renderer, words, r->ringWords, &r->prims, &why);
	if (r->words == 0)
	{
		vk_compute_drawer_fallback(d, why);
		return false;
	}
	r->gpu = pvrrc.gpu;
	r->dummyView = vk_texture_view(paletteTexture);

	/* (the buffers and textures the set has are written by record, after
	 * the texture a render to a texture goes to is taken, which the frame's
	 * textures can be: as the raster renderer has them) */
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

static void image_barrier(VkImageMemoryBarrier *b, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to, VkImage image)
{
	memset(b, 0, sizeof(*b));
	b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b->srcAccessMask = src;
	b->dstAccessMask = dst;
	b->oldLayout = from;
	b->newLayout = to;
	b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->image = image;
	b->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b->subresourceRange.levelCount = 1;
	b->subresourceRange.layerCount = 1;
}

/* (a render to a texture is drawn at one sample a pixel, and stores no
 * sums: the kernels of its own, where there are) */
static VkPipeline pipeline_of(vk_compute_res *r, int k)
{
	return r->rtt && has(r, 1, k) ? kernel(r, 1, k) : kernel(r, 0, k);
}

#define STEP_BARRIER(cmd) memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, \
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT, \
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT)

/* a kernel, @x by @y of it (none if either is 0), and what it wrote made what the next reads */
static bool step(vk_compute_res *r, VkCommandBuffer cmd, int k, u32 x, u32 y)
{
	const VkPipeline p = pipeline_of(r, k);
	if (p == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	if (x != 0 && y != 0)
		vkCmdDispatch(cmd, x, y, 1);
	STEP_BARRIER(cmd);
	return true;
}

/* ...as many as the bins' counts at @at say */
static bool indirect(vk_compute_res *r, VkCommandBuffer cmd, int k, u32 at)
{
	const VkPipeline p = pipeline_of(r, k);
	if (p == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdDispatchIndirect(cmd, r->bins.buffer, (VkDeviceSize)(PVRC_B_STATS(r->runs) + at) * 4);
	STEP_BARRIER(cmd);
	return true;
}

/* The set's textures, picture and palette, where they changed; and what
 * the frame's vertices, indices and volumes are read from: the ring's
 * slot, or the decode's buffers (whose handles change only when it is
 * made again) */
static void write_set(vk_compute_drawer *d, int slot, VkImageView view, VkImageView last)
{
	vk_compute_res *r = d->res;
	const VkDescriptorSet set = r->sets[slot];
	set_state *state = &r->setStates[slot];
	const VkBuffer ring = r->ring.buffer;
	const VkImageView dummyView = r->dummyView;
	const VkSampler dummySampler = r->nearest;
	VkBuffer want[GPU_BINDINGS];
	u32 i, most;
	int b;

	r->nImages = r->nWrites = r->nBuffers = 0;
	if (r->gpu)
	{
		want[0] = d->gpuDraw->vertices;
		want[1] = d->gpuDraw->indices;
		want[2] = d->gpuDraw->modtrig;
		want[3] = d->gpuDraw->commands;
		want[4] = d->gpuDraw->trDraws;
		want[5] = d->gpuDraw->frame;
	}
	else
		for (b = 0; b < GPU_BINDINGS; b++)
			want[b] = ring;
	for (b = 0; b < GPU_BINDINGS; b++)
	{
		if (state->buffers[b] == want[b])
			continue;
		if (want[b] == ring)
			buffer_write(&r->writes[r->nWrites++], &r->bufferInfos[r->nBuffers++], set, (u32)(B_VTX + b), ring,
					(VkDeviceSize)slot * r->ringWords * 4, (VkDeviceSize)r->ringWords * 4);
		else
			buffer_write(&r->writes[r->nWrites++], &r->bufferInfos[r->nBuffers++], set, (u32)(B_VTX + b), want[b], 0, VK_WHOLE_SIZE);
		state->buffers[b] = want[b];
	}
	image_write(&r->writes[r->nWrites++], &r->imageInfos[r->nImages++], set, B_PAL, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			dummySampler, dummyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	most = r->slotTable.used > state->used ? r->slotTable.used : state->used;
	for (i = 0; i < most; i++)
	{
		VkImageView v = dummyView;
		VkSampler s = dummySampler;
		if (i < r->slotTable.used)
		{
			v = vk_texture_read_only_view((const vk_texture *)(uintptr_t)r->slotTable.slotTex[i]);
			s = vk_samplers_get(d->samplers, r->slotTable.slotTsp[i]);
		}
		/* (a slot this frame uses is written whatever it had: a view made in
		 * the place of one gone can have its handle) */
		if (i < r->slotTable.used || state->views[i] != v || state->samplers[i] != s)
		{
			image_write(&r->writes[r->nWrites++], &r->imageInfos[r->nImages++], set, B_TEX, i,
					VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, s, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			state->views[i] = v;
			state->samplers[i] = s;
		}
	}
	state->used = r->slotTable.used;
	/* (the pictures and the palette every frame: three words) */
	image_write(&r->writes[r->nWrites++], &r->imageInfos[r->nImages++], set, B_OUT, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL);
	image_write(&r->writes[r->nWrites++], &r->imageInfos[r->nImages++], set, B_LAST, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			last != VK_NULL_HANDLE && r->lastScaled ? r->linear : dummySampler, last != VK_NULL_HANDLE ? last : dummyView,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkUpdateDescriptorSets(vkc.device, r->nWrites, r->writes, 0, NULL);
}

/* The bins' counts and the counts for the log, at 0 (the pool is 0 after
 * the frame before's clear), after the frame before is done with them;
 * and the walk's variants' dispatches across */
static void clear_bins(vk_compute_drawer *d, VkCommandBuffer cmd, const u32 *words)
{
	vk_compute_res *r = d->res;
	const VkBuffer bins = r->bins.buffer;
	/* (only this frame's runs: the bins have room for the largest texture
	 * rendered to's, and the kernels read no others) */
	const u32 used = (r->gpu ? r->chunks : words[PVRC_F_CHUNKS]) * words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	/* (the walk's variants' dispatches across: the CPU's; the sort counts the tiles of each) */
	const u32 across = pvrc_walk_x(words[PVRC_F_TILE_LOG2], r->rtt ? 1 : d->samples);

	memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	if (!r->binsZero)
		vkCmdFillBuffer(cmd, bins, 0, VK_WHOLE_SIZE, 0);
	else if (used >= r->runs)
		vkCmdFillBuffer(cmd, bins, 0, (VkDeviceSize)PVRC_B_POOL(r->runs) * 4, 0);
	else
	{
		if (used != 0)
		{
			vkCmdFillBuffer(cmd, bins, (VkDeviceSize)PVRC_B_COUNT(r->runs) * 4, (VkDeviceSize)used * 4, 0);
			vkCmdFillBuffer(cmd, bins, (VkDeviceSize)PVRC_B_START(r->runs) * 4, (VkDeviceSize)used * 4, 0);
			vkCmdFillBuffer(cmd, bins, (VkDeviceSize)PVRC_B_CURSOR(r->runs) * 4, (VkDeviceSize)used * 4, 0);
		}
		vkCmdFillBuffer(cmd, bins, (VkDeviceSize)PVRC_B_STATS(r->runs) * 4,
				(VkDeviceSize)(PVRC_B_POOL(r->runs) - PVRC_B_STATS(r->runs)) * 4, 0);
	}
	r->binsZero = true;
	memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT);
	vkCmdFillBuffer(cmd, bins, (VkDeviceSize)(PVRC_B_STATS(r->runs) + PVRC_ST_WALK_U_X) * 4, 4, across);
	vkCmdFillBuffer(cmd, bins, (VkDeviceSize)(PVRC_B_STATS(r->runs) + PVRC_ST_WALK_S_X) * 4, 4, across);
}

/* the kernels, in turn */
static bool dispatches(vk_compute_drawer *d, VkCommandBuffer cmd, const u32 *words, u32 *yfilter)
{
	vk_compute_res *r = d->res;
	const u32 tiles = words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	const u32 groups = (r->prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP;
	VkPipeline p;
	u32 yx, yy;

	if (r->gpu)
	{
		/* A frame of the decode's: its numbering, and the dispatches of the
		 * kernels whose sizes are the GPU's, from it */
		if (!step(r, cmd, PVRC_K_GPUSRC, 1, 1) || !indirect(r, cmd, PVRC_K_SETUP, PVRC_ST_SETUP_X)
				|| !step(r, cmd, PVRC_K_SCAN, 1, 1) || !indirect(r, cmd, PVRC_K_SCATTER, PVRC_ST_SETUP_X)
				|| !indirect(r, cmd, PVRC_K_SORT, PVRC_ST_SORT_X))
			return false;
	}
	else if (!step(r, cmd, PVRC_K_SETUP, groups, 1) || !step(r, cmd, PVRC_K_SCAN, 1, 1)
			|| !step(r, cmd, PVRC_K_SCATTER, groups, 1) || !step(r, cmd, PVRC_K_SORT, tiles, words[PVRC_F_CHUNKS]))
		return false;
	/* (the walk's two variants, each over the coarse tiles of its list,
	 * the sort's: nothing between them) */
	p = pipeline_of(r, WALK_UNSORTED);
	if (p == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdDispatchIndirect(cmd, r->bins.buffer, (VkDeviceSize)(PVRC_B_STATS(r->runs) + PVRC_ST_WALK_U_X) * 4);
	p = pipeline_of(r, PVRC_K_WALK);
	if (p == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdDispatchIndirect(cmd, r->bins.buffer, (VkDeviceSize)(PVRC_B_STATS(r->runs) + PVRC_ST_WALK_S_X) * 4);
	if (!step(r, cmd, PVRC_K_WALK, 0, 0))
		return false;
	/* the Y filter, in place over the picture, where the frame has it */
	pvrc_yfilter_groups(words, &yx, &yy);
	if (yy != 0)
	{
		p = pipeline_of(r, PVRC_K_YFILTER);
		if (p == VK_NULL_HANDLE)
			return false;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
		vkCmdDispatch(cmd, yx, yy, 1);
	}
	*yfilter = yy;
	p = pipeline_of(r, PVRC_K_CLEAR);
	if (p == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdDispatchIndirect(cmd, r->bins.buffer, (VkDeviceSize)(PVRC_B_STATS(r->runs) + PVRC_ST_CLEAR_X) * 4);
	return true;
}

bool vk_compute_drawer_record(vk_compute_drawer *d, VkImage image, VkImageView view, VkImageView last)
{
	vk_compute_res *r = d->res;
	const int slot = d->pool->index;
	const u32 *words = r->ringMap + (size_t)slot * r->ringWords;
	const u32 n = r->rtt ? 1 : d->samples;
	const bool depthStored = d->depth && !r->rtt;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier toGeneral[3], toRead;
	VkMemoryBarrier toCompute;
	VkBufferCopy copy;
	VkCommandBuffer cmd;
	u32 yy = 0;

	write_set(d, slot, view, last);

	/* The dispatches */
	cmd = vk_cmd_pool_allocate(d->pool);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	vk_gpu_timer_begin(&d->timer, cmd, slot);
	clear_bins(d, cmd, words);
	/* (and what the frame's textures were made with before: the texture
	 * decoder's and the uploads' barriers, and a render to texture's render
	 * pass, make them ready for fragment shaders, not these) */
	memset(&toCompute, 0, sizeof(toCompute));
	toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	toCompute.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
	image_barrier(&toGeneral[0], 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, image);
	/* (the slot's depth, written whole by the walk, read by the frame before's upscaler until now) */
	image_barrier(&toGeneral[1], 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			r->depths[d->depth ? slot : 0].image);
	/* (the sums, the first time: general from then on) */
	image_barrier(&toGeneral[2], 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, r->sums.image);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
			0, 1, &toCompute, 0, NULL, r->sumsLayout ? 2 : 3, toGeneral);
	r->sumsLayout = true;
	d->recordedSlot = slot;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipelineLayout, 0, 1, &r->sets[slot], 0, NULL);
	if (!dispatches(d, cmd, words, &yy))
		return false;
	/* (the sums are the picture's where it was written by the walk alone:
	 * not where the write-out has been over it since; and what they add is
	 * more than 8 bits, which a pixel of one sample does not have) */
	r->sumsStored = r->sumsOn && !r->rtt && yy == 0 && (n > 1 || (words[PVRC_F_FLAGS] & PVRC_FF_HSCALE) != 0);
	/* the counts, for when this slot comes round */
	memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT);
	copy.srcOffset = (VkDeviceSize)PVRC_B_STATS(r->runs) * 4;
	copy.dstOffset = (VkDeviceSize)slot * PVRC_STATS * 4;
	copy.size = PVRC_STATS * 4;
	vkCmdCopyBuffer(cmd, r->bins.buffer, r->stats.buffer, 1, &copy);
	r->statsWritten[slot] = true;
	/* the picture, for the frontend, as the raster renderer leaves it; a
	 * texture, for the frames that draw with it (either renderer) and the
	 * copy to video memory when the game touches it */
	image_barrier(&toRead, VK_ACCESS_SHADER_WRITE_BIT,
			r->rtt ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			r->rtt ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
				: VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toRead);
	if (depthStored)
	{
		/* the depth, for the motion vector pass and the upscaler */
		VkImageMemoryBarrier depthRead;
		image_barrier(&depthRead, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r->depths[slot].image);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &depthRead);
		if (d->motion != NULL && d->motion->recorded && !r->gpu)
		{
			/* (the frame's vertices and indices, in its words; its pixel clip) */
			const VkDeviceSize base = (VkDeviceSize)slot * r->ringWords * 4;
			VkRect2D scissor;
			scissor.offset.x = (s32)words[PVRC_F_SCISSOR];
			scissor.offset.y = (s32)words[PVRC_F_SCISSOR + 1];
			scissor.extent.width = words[PVRC_F_SCISSOR + 2] - words[PVRC_F_SCISSOR];
			scissor.extent.height = words[PVRC_F_SCISSOR + 3] - words[PVRC_F_SCISSOR + 1];
			if (!vk_motion_pass_record(d->motion, cmd, slot, r->ring.buffer, base + (VkDeviceSize)words[PVRC_F_O_IDX] * 4, &scissor,
					base + (VkDeviceSize)words[PVRC_F_O_VTX] * 4))
				return false;
		}
	}
	vk_gpu_timer_end(&d->timer, cmd, slot);
	vkEndCommandBuffer(cmd);
	d->statFrames++;
	if (r->gpu)
		d->statGpuFrames++;
	if (r->rtt)
		d->statRttFrames++;
	return true;
}
