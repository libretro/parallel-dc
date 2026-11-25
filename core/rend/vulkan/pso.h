/* The raster renderers' pipelines, made without a frame waiting for one.
 *
 * A polygon's pipeline is kept under a key: everything the pipeline is made
 * of, worked out from the polygon (PipelineManager::Key(),
 * OITPipelineManager::Key()). The first time a key is drawn its pipeline is
 * asked of the worker thread below, and the polygon is drawn at once with
 * the renderer's general pipeline for it instead: the same shader with its
 * bits read from the push constants (pso_key.h) and, where the device has
 * them (VK_EXT_extended_dynamic_state and _state3), the depth, stencil,
 * cull, topology and blend states set with the draw. It draws the same
 * pixels. The general pipelines, the modifier volumes' and the other fixed
 * ones are handed to the worker when the renderer starts (BuildSoon()),
 * without waiting for them: a draw that needs one first takes it made, or
 * makes it there and then if the worker has not started on it, or waits for
 * the worker if it is making it (PsoGet()). So the renderer starts at once,
 * and a draw waits only for a fixed pipeline it needs before the worker got
 * to it.
 *
 * A device without the dynamic states has a general pipeline for each of
 * those states it is drawn with; one not made yet (a state not seen in the
 * pipelines kept from earlier runs, PsoKeyLog) is made there and then -
 * the one case a draw waits, and only once for each such state.
 *
 * Nothing here takes a lock: a slot's state is an atomic the worker
 * publishes the pipeline with; the worker's requests are an intrusive list
 * (lock-free, intrusive), and it sleeps on an eventcount. The worker has a pipeline
 * cache of its own, merged into the context's when that is saved. Every
 * table is made when its renderer is; nothing is allocated in a frame. */
#ifndef VK_PSO_H
#define VK_PSO_H

#include <stddef.h>
#include <retro_atomic.h>
#include "vulkan.h"
#include "log/Log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_pso_builder vk_pso_builder;
typedef struct PsoSlot PsoSlot;

/* BUILDING: taken by one thread to make - the worker or a draw - so that it
 * is made once */
enum PsoState { PSO_EMPTY, PSO_QUEUED, PSO_READY, PSO_FAILED, PSO_BUILDING };

struct PsoSlot
{
	u64 key;
	vk_pso_builder *owner;
	retro_atomic_int_t state;	/* PsoState: the worker publishes READY or FAILED */
	int gen;					/* the owner's generation it was asked for in */
	VkPipeline pipeline;
	PsoSlot *next;				/* the worker's lists */
};

/* What makes the pipelines of its slots' keys: a renderer's pipeline
 * manager. build() is called from the worker thread too, and reads only
 * what the manager does not change while it has slots asked for (Reset()
 * waits for the worker first). It returns VK_NULL_HANDLE if the pipeline
 * could not be made. */
struct vk_pso_builder
{
	VkPipeline (*build)(vk_pso_builder *self, u64 key, VkPipelineCache cache);
	retro_atomic_int_t generation;
};

static INLINE int vk_pso_builder_generation(const vk_pso_builder *b)
{
	return retro_atomic_load_acquire_int(&((vk_pso_builder *)b)->generation);
}

/* what has been asked for so far is not to be made any more */
static INLINE void vk_pso_builder_new_generation(vk_pso_builder *b)
{
	retro_atomic_fetch_add_int(&b->generation, 1);
}

/* A manager's pipelines by key: open addressing, made once. Only the
 * thread that draws looks keys up and adds them. */
typedef struct vk_pso_table
{
	PsoSlot *slots;
	vk_pso_builder *owner;
	u32 mask;
	u32 used;
} vk_pso_table;

void vk_pso_table_init(vk_pso_table *t, u32 capacity, vk_pso_builder *owner);
void vk_pso_table_free(vk_pso_table *t);

static INLINE u32 vk_pso_table_hash(const vk_pso_table *t, u64 key)
{
	return (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & t->mask;
}

static INLINE PsoSlot *vk_pso_table_find(const vk_pso_table *t, u64 key)
{
	u32 i;
	for (i = vk_pso_table_hash(t, key);; i = (i + 1) & t->mask)
	{
		PsoSlot *s = &t->slots[i];
		if (!s->owner)
			return NULL;
		if (s->key == key)
			return s;
	}
}

/* NULL when the table is full */
PsoSlot *vk_pso_table_insert(vk_pso_table *t, u64 key);
/* The pipelines destroyed and the table emptied. Nothing of it may be
 * with the worker (vk_pso_worker_wait_idle()). */
void vk_pso_table_clear(vk_pso_table *t, VkDevice device);

/* a graphics pipeline, or VK_NULL_HANDLE (and a line in the log) if the device could not make it */
VkPipeline vk_pso_create(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *info);
/* a compute pipeline, or VK_NULL_HANDLE (and a line in the log) */
VkPipeline vk_pso_create_compute(VkDevice device, VkPipelineCache cache, const VkComputePipelineCreateInfo *info);

/* The most general pipelines a manager makes at its start, for a device
 * without the dynamic states (PipelineManager::Prepare()) */
enum { PSO_GENERAL_AT_START = 1024 };

/* (tests: every polygon drawn with the general pipelines, to see they draw
 * what the polygons' own ones do) */
#ifdef VK_PSO_TEST_GENERAL
#define PsoTestGeneral 1
#else
#define PsoTestGeneral 0
#endif

static INLINE bool PsoReady(const PsoSlot *slot)
{
	return slot != NULL && retro_atomic_load_acquire_int(&((PsoSlot *)slot)->state) == PSO_READY;
}

/* The keys of the pipelines made, kept with the pipeline cache from one run
 * to the next (vk_context.h): at its start a renderer asks the worker for
 * all of them that fit how it draws now. @who: which manager's. */
enum { VK_PSO_KEYS_CAPACITY = 8192 };

typedef struct vk_pso_keys
{
	u64 *keys;
	u32 count;
} vk_pso_keys;

void vk_pso_keys_init(vk_pso_keys *k);
void vk_pso_keys_term(vk_pso_keys *k);

static INLINE void vk_pso_keys_record(vk_pso_keys *k, u32 who, u64 key)
{
	if (k->count < VK_PSO_KEYS_CAPACITY)
		k->keys[k->count++] = ((u64)who << 60) | key;
}

/* read from / written to the cache file: the keys, sorted, each once */
const u64 *vk_pso_keys_sorted(vk_pso_keys *k, u32 *n);
void vk_pso_keys_load(vk_pso_keys *k, const void *keys, u32 n);

/* The worker thread.
 * start: @initial_data, the pipeline cache's, given to the worker's cache
 * too; @externally_synced: its caches are made with
 * VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT (no driver lock).
 * stop: the thread joined; its cache merged into @into and destroyed. */
void vk_pso_worker_start(VkDevice device, const void *initial_data, size_t size, bool externally_synced);
void vk_pso_worker_stop(VkPipelineCache into);
/* Without it (a build without threads) nothing is asked of it: the
 * polygons are drawn with the general pipelines */
bool vk_pso_worker_running(void);
/* the drawing thread: @slot is a new one of its table; made soon */
void vk_pso_worker_request(PsoSlot *slot);
/* made when nothing more pressing is asked */
void vk_pso_worker_prefetch(PsoSlot *slot);
/* @n slots given to the worker to make before anything else, not waited
 * for: a draw takes each with PsoGet(). (Without the worker: made now,
 * with @cache.) Slots stay the batch's until vk_pso_worker_end_batch(). */
void vk_pso_worker_build_soon(PsoSlot *const *slots, u32 n, VkPipelineCache cache);
/* The batch done with, before @owner's slots go: what is left of @owner's
 * is dropped, what is left of the others' made now with the draw cache
 * (@owner NULL: everything left dropped); back when the worker has left
 * the batch. */
void vk_pso_worker_end_batch(const vk_pso_builder *owner);
/* The cache a draw that makes a fixed pipeline itself makes it with: the
 * context's (only the drawing thread uses it) */
void vk_pso_worker_set_draw_cache(VkPipelineCache cache);
/* A slot of the batch not made yet, for a draw: made now on this thread
 * if the worker has not taken it, else waited for. Its pipeline, or
 * VK_NULL_HANDLE if it was never asked for or could not be made. */
VkPipeline vk_pso_worker_need(PsoSlot *slot);
/* until all that was asked so far is made or dropped */
void vk_pso_worker_wait_idle(void);
/* Until every slot of the batch is made (by the worker: this thread only
 * waits). The start's, before the first frame: a draw then takes them
 * made, and never makes one itself. */
void vk_pso_worker_wait_batch(void);
/* whether every slot of the batch is made, without waiting */
bool vk_pso_worker_batch_ready(void);
/* the worker's cache merged into @into (the worker idle) */
void vk_pso_worker_merge_into(VkPipelineCache into);
/* One slot's pipeline made with @cache, and published. (All of the
 * renderers' graphics pipelines are made here: one place to count.) */
void vk_pso_worker_build(PsoSlot *slot, VkPipelineCache cache);
/* A draw that cannot go without @slot's pipeline: made on this thread
 * now. The slot is not one the worker has. */
void vk_pso_worker_build_sync(PsoSlot *slot, VkPipelineCache cache);
u32 vk_pso_worker_built(void);
u32 vk_pso_worker_sync_built(void);

/* Slots of a builder's own, not a table's: the device's utility pipelines
 * (quad.h, ssaa.h, tex_decoder.h, vk_compute_drawer.c), handed to the worker
 * with vk_pso_worker_build_soon() where the device made them before. Init:
 * @n of them, empty, keyed 0 to @n - 1. Drop: what the worker has of them
 * dropped, the pipelines destroyed, empty again (nothing done if they are).
 * Made: a pipeline the drawing thread made itself, published. */
void PsoOwnInit(vk_pso_builder *owner, PsoSlot *slots, u32 n);
void PsoOwnDrop(vk_pso_builder *owner, PsoSlot *slots, u32 n, VkDevice device);
VkPipeline PsoOwnMade(PsoSlot *slot, VkPipeline pipeline);

/* A fixed slot's pipeline for a draw (vk_pso_worker_build_soon()): once it
 * is made, an acquire load and a compare more than reading it */
static INLINE VkPipeline vk_pso_get(PsoSlot *slot)
{
	if (retro_atomic_load_acquire_int(&slot->state) == PSO_READY)
		return slot->pipeline;
	return vk_pso_worker_need(slot);
}

/* Device functions of VK_EXT_extended_dynamic_state and _state3 the general
 * pipelines are drawn with (vk_context.h has them when the device does);
 * the Vulkan headers here are older than both */
enum
{
	VK_PSO_DYNAMIC_STATE_CULL_MODE = 1000267000,
	VK_PSO_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY = 1000267002,
	VK_PSO_DYNAMIC_STATE_DEPTH_TEST_ENABLE = 1000267006,
	VK_PSO_DYNAMIC_STATE_DEPTH_WRITE_ENABLE = 1000267007,
	VK_PSO_DYNAMIC_STATE_DEPTH_COMPARE_OP = 1000267008,
	VK_PSO_DYNAMIC_STATE_STENCIL_TEST_ENABLE = 1000267010,
	VK_PSO_DYNAMIC_STATE_STENCIL_OP = 1000267011,
	VK_PSO_DYNAMIC_STATE_COLOR_BLEND_ENABLE = 1000455010,
	VK_PSO_DYNAMIC_STATE_COLOR_BLEND_EQUATION = 1000455011,
	VK_PSO_DYNAMIC_STATE_COLOR_WRITE_MASK = 1000455012,
	VK_PSO_STRUCTURE_TYPE_EDS_FEATURES = 1000267000,
	VK_PSO_STRUCTURE_TYPE_EDS3_FEATURES = 1000455000,
	VK_PSO_STRUCTURE_TYPE_CACHE_CONTROL_FEATURES = 1000297000,
	VK_PSO_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED = 1
};

typedef struct vk_pso_eds_features
{
	int sType;
	void *pNext;
	VkBool32 extendedDynamicState;
} vk_pso_eds_features;

typedef struct vk_pso_eds3_features
{
	int sType;
	void *pNext;
	VkBool32 tessellationDomainOrigin, depthClampEnable, polygonMode, rasterizationSamples, sampleMask,
		alphaToCoverageEnable, alphaToOneEnable, logicOpEnable, colorBlendEnable, colorBlendEquation,
		colorWriteMask, rasterizationStream, conservativeRasterizationMode, extraPrimitiveOverestimationSize,
		depthClipEnable, sampleLocationsEnable, colorBlendAdvanced, provokingVertexMode, lineRasterizationMode,
		lineStippleEnable, depthClipNegativeOneToOne, viewportWScalingEnable, viewportSwizzle,
		coverageToColorEnable, coverageToColorLocation, coverageModulationMode, coverageModulationTableEnable,
		coverageModulationTable, coverageReductionMode, representativeFragmentTestEnable, shadingRateImageEnable;
} vk_pso_eds3_features;

typedef struct vk_pso_cache_control_features
{
	int sType;
	void *pNext;
	VkBool32 pipelineCreationCacheControl;
} vk_pso_cache_control_features;

typedef struct vk_pso_blend_equation
{
	VkBlendFactor srcColorBlendFactor;
	VkBlendFactor dstColorBlendFactor;
	VkBlendOp colorBlendOp;
	VkBlendFactor srcAlphaBlendFactor;
	VkBlendFactor dstAlphaBlendFactor;
	VkBlendOp alphaBlendOp;
} vk_pso_blend_equation;

typedef struct vk_pso_dyn
{
	void (VKAPI_PTR *CmdSetCullMode)(VkCommandBuffer, VkCullModeFlags);
	void (VKAPI_PTR *CmdSetPrimitiveTopology)(VkCommandBuffer, VkPrimitiveTopology);
	void (VKAPI_PTR *CmdSetDepthTestEnable)(VkCommandBuffer, VkBool32);
	void (VKAPI_PTR *CmdSetDepthWriteEnable)(VkCommandBuffer, VkBool32);
	void (VKAPI_PTR *CmdSetDepthCompareOp)(VkCommandBuffer, VkCompareOp);
	void (VKAPI_PTR *CmdSetStencilTestEnable)(VkCommandBuffer, VkBool32);
	void (VKAPI_PTR *CmdSetStencilOp)(VkCommandBuffer, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp);
	void (VKAPI_PTR *CmdSetColorBlendEnable)(VkCommandBuffer, uint32_t, uint32_t, const VkBool32 *);
	void (VKAPI_PTR *CmdSetColorBlendEquation)(VkCommandBuffer, uint32_t, uint32_t, const vk_pso_blend_equation *);
	void (VKAPI_PTR *CmdSetColorWriteMask)(VkCommandBuffer, uint32_t, uint32_t, const VkColorComponentFlags *);
	bool depthStencil;	/* VK_EXT_extended_dynamic_state: cull, topology, depth, stencil */
	bool blend;			/* VK_EXT_extended_dynamic_state3: blend enable, equation, write mask */
} vk_pso_dyn;

/* The cull mode of the fixed pipelines made with it dynamic: the modifier
 * volumes' and the depth prepass's, where the device has
 * VK_EXT_extended_dynamic_state (one pipeline then for the three modes).
 * PsoCullIndex(): a polygon's ISP cull mode as 0 none, 1 front, 2 back,
 * which is VkCullModeFlags too. */
static INLINE u32 PsoCullIndex(u32 ispCullMode)
{
	return ispCullMode >= 2 ? ispCullMode - 1 : 0;
}

/* Set with the draws of one block of those pipelines, only when it changes.
 * Made at the block's start: what was set before is not known (the general
 * pipelines set their own). @set null (the device has no dynamic cull
 * mode, and the pipelines one each): it never sets any - the same compare,
 * never true. */
typedef struct vk_pso_cull
{
	VkCommandBuffer cmd;
	void (VKAPI_PTR *set)(VkCommandBuffer, VkCullModeFlags);
	u32 mask;
	u32 last;
} vk_pso_cull;

static INLINE void vk_pso_cull_init(vk_pso_cull *c, VkCommandBuffer cmd, void (VKAPI_PTR *set)(VkCommandBuffer, VkCullModeFlags))
{
	c->cmd = cmd;
	c->set = set;
	c->mask = set != NULL ? 3 : 0;
	c->last = set != NULL ? ~0u : 0;
}

/* @cull: PsoCullIndex()'s */
static INLINE void vk_pso_cull_set(vk_pso_cull *c, u32 cull)
{
	const u32 m = cull & c->mask;
	if (m != c->last)
	{
		c->last = m;
		c->set(c->cmd, (VkCullModeFlags)m);
	}
}

#ifdef __cplusplus
}
#endif

#endif
