/* See pso.h. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE	/* syscall() */
#endif
#include "pso.h"
#include <stdlib.h>
#include <string.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

enum { BATCH_CAPACITY = 4096 };

/* The worker: one, as the context is */
static struct
{
	sthread_t *thread;
	VkDevice device;
	VkPipelineCache cache;
	retro_atomic_ptr_t urgent;		/* intrusive lists, newest first, through PsoSlot::next */
	retro_atomic_ptr_t background;
	retro_eventcount_t work;	/* the worker sleeps here */
	retro_eventcount_t idle;	/* and who waits for it, here */
	bool events;				/* the two above made */
	bool inited;
	retro_atomic_int_t stop;
	retro_atomic_int_t requested;	/* written by the drawing thread only */
	retro_atomic_int_t answered;
	retro_atomic_int_t built;
	u32 syncBuilt;

	/* vk_pso_worker_build_soon()'s slots, shared out between the two
	 * threads: entries [0, batchSize) are published (the drawing thread
	 * appends them), [batchNext, batchSize) not taken yet; and room for
	 * what end_batch() keeps of them */
	PsoSlot **batch;
	PsoSlot **keep;
	retro_atomic_int_t batchSize;
	retro_atomic_int_t batchActive;
	retro_atomic_int_t batchNext;
	retro_atomic_int_t batchInside;
	VkPipelineCache drawCache;
	PsoSlot *waitingFor;	/* need()'s, for not_building() */
} w;

void vk_pso_table_init(vk_pso_table *t, u32 capacity, vk_pso_builder *owner)
{
	if (!t->slots)
	{
		u32 i, n = 16;
		while (n < capacity)
			n <<= 1;
		t->slots = (PsoSlot *)calloc(n, sizeof(PsoSlot));
		if (t->slots == NULL)
			abort();
		t->mask = n - 1;
		for (i = 0; i < n; i++)
		{
			t->slots[i].owner = NULL;
			t->slots[i].pipeline = VK_NULL_HANDLE;
			t->slots[i].next = NULL;
			retro_atomic_int_init(&t->slots[i].state, PSO_EMPTY);
		}
	}
	t->owner = owner;
}

void vk_pso_table_free(vk_pso_table *t)
{
	free(t->slots);
	t->slots = NULL;
	t->mask = 0;
	t->used = 0;
}

PsoSlot *vk_pso_table_insert(vk_pso_table *t, u64 key)
{
	u32 i;
	PsoSlot *s;
	/* kept under three quarters full: find() always meets an empty slot */
	if (t->used >= (t->mask + 1) / 4 * 3)
		return NULL;
	i = vk_pso_table_hash(t, key);
	while (t->slots[i].owner)
		i = (i + 1) & t->mask;
	s = &t->slots[i];
	s->key = key;
	s->owner = t->owner;
	s->pipeline = VK_NULL_HANDLE;
	s->gen = vk_pso_builder_generation(t->owner);
	retro_atomic_store_relaxed_int(&s->state, PSO_EMPTY);
	t->used++;
	return s;
}

void vk_pso_table_clear(vk_pso_table *t, VkDevice device)
{
	u32 i;
	if (t->slots == NULL)
		return;
	for (i = 0; i <= t->mask; i++)
	{
		PsoSlot *s = &t->slots[i];
		if (!s->owner)
			continue;
		if (s->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(device, s->pipeline, NULL);
		s->pipeline = VK_NULL_HANDLE;
		s->owner = NULL;
		retro_atomic_store_relaxed_int(&s->state, PSO_EMPTY);
	}
	t->used = 0;
}

VkPipeline vk_pso_create(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *info)
{
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult res = vkCreateGraphicsPipelines(device, cache, 1, info, NULL, &pipeline);
	if (res != VK_SUCCESS)
	{
		WARN_LOG(RENDERER, "Vulkan: a pipeline could not be made (%d)\n", (int)res);
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

VkPipeline vk_pso_create_compute(VkDevice device, VkPipelineCache cache, const VkComputePipelineCreateInfo *info)
{
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult res = vkCreateComputePipelines(device, cache, 1, info, NULL, &pipeline);
	if (res != VK_SUCCESS)
	{
		WARN_LOG(RENDERER, "Vulkan: a pipeline could not be made (%d)\n", (int)res);
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

void PsoOwnInit(vk_pso_builder *owner, PsoSlot *slots, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
	{
		slots[i].key = (u64)i;
		slots[i].owner = owner;
		slots[i].gen = 0;
		slots[i].pipeline = VK_NULL_HANDLE;
		slots[i].next = NULL;
		retro_atomic_int_init(&slots[i].state, PSO_EMPTY);
	}
}

void PsoOwnDrop(vk_pso_builder *owner, PsoSlot *slots, u32 n, VkDevice device)
{
	u32 i;
	for (i = 0; i < n; i++)
		if (retro_atomic_load_relaxed_int(&slots[i].state) != PSO_EMPTY)
			break;
	if (i == n)
		return;
	vk_pso_worker_end_batch(owner);
	for (i = 0; i < n; i++)
	{
		if (slots[i].pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(device, slots[i].pipeline, NULL);
		slots[i].pipeline = VK_NULL_HANDLE;
		retro_atomic_store_relaxed_int(&slots[i].state, PSO_EMPTY);
	}
}

VkPipeline PsoOwnMade(PsoSlot *slot, VkPipeline pipeline)
{
	slot->pipeline = pipeline;
	retro_atomic_store_release_int(&slot->state, PSO_READY);
	return pipeline;
}

void vk_pso_keys_init(vk_pso_keys *k)
{
	if (!k->keys)
	{
		k->keys = (u64 *)malloc(VK_PSO_KEYS_CAPACITY * sizeof(u64));
		if (k->keys == NULL)
			abort();
	}
	k->count = 0;
}

void vk_pso_keys_term(vk_pso_keys *k)
{
	free(k->keys);
	k->keys = NULL;
	k->count = 0;
}

static int key_order(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

const u64 *vk_pso_keys_sorted(vk_pso_keys *k, u32 *n)
{
	if (k->keys && k->count != 0)
	{
		u32 i, out = 1;
		qsort(k->keys, k->count, sizeof(u64), key_order);
		for (i = 1; i < k->count; i++)
			if (k->keys[i] != k->keys[out - 1])
				k->keys[out++] = k->keys[i];
		k->count = out;
	}
	*n = k->count;
	return k->keys;
}

void vk_pso_keys_load(vk_pso_keys *k, const void *from, u32 n)
{
	vk_pso_keys_init(k);
	k->count = n < VK_PSO_KEYS_CAPACITY ? n : VK_PSO_KEYS_CAPACITY;
	if (k->count != 0)
		memcpy(k->keys, from, k->count * sizeof(u64));
}

static void list_push(retro_atomic_ptr_t *head, PsoSlot *p)
{
	for (;;)
	{
		void *old = retro_atomic_load_relaxed_ptr(head);
		p->next = (PsoSlot *)old;
		if (retro_atomic_cas_ptr(head, old, p))
			return;
	}
}

/* Consumer only: everything pushed so far, oldest first */
static PsoSlot *list_take_all(retro_atomic_ptr_t *head)
{
	PsoSlot *list = (PsoSlot *)retro_atomic_exchange_ptr(head, NULL);
	PsoSlot *out = NULL;
	while (list != NULL)
	{
		PsoSlot *next = list->next;
		list->next = out;
		out = list;
		list = next;
	}
	return out;
}

static void notify(retro_eventcount_t *ec)
{
	if (w.events)
		retro_eventcount_notify(ec);
}

static VkPipelineCache make_cache(VkDevice device, const void *data, size_t size, bool externallySynced)
{
	VkPipelineCacheCreateInfo info;
	VkPipelineCache cache = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	info.flags = externallySynced ? VK_PSO_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED : 0;
	info.initialDataSize = size;
	info.pInitialData = data;
	if (vkCreatePipelineCache(device, &info, NULL, &cache) != VK_SUCCESS && size != 0)
	{
		/* (data the device will not take: an empty cache) */
		info.initialDataSize = 0;
		info.pInitialData = NULL;
		vkCreatePipelineCache(device, &info, NULL, &cache);
	}
	return cache;
}

static void worker_loop(void *arg);

void vk_pso_worker_start(VkDevice device, const void *initialData, size_t size, bool externallySynced)
{
	if (!w.inited)
	{
		w.inited = true;
		retro_atomic_ptr_init(&w.urgent, NULL);
		retro_atomic_ptr_init(&w.background, NULL);
		retro_atomic_int_init(&w.built, 0);
		retro_atomic_int_init(&w.batchSize, 0);
		retro_atomic_int_init(&w.batchActive, 0);
		retro_atomic_int_init(&w.batchNext, 0);
		retro_atomic_int_init(&w.batchInside, 0);
		retro_atomic_int_init(&w.stop, 0);
		retro_atomic_int_init(&w.requested, 0);
		retro_atomic_int_init(&w.answered, 0);
	}
#ifndef TARGET_NO_THREADS
	if (!w.events)
	{
		if (!retro_eventcount_init(&w.work) || !retro_eventcount_init(&w.idle))
			abort();
		w.events = true;
	}
#endif
	w.device = device;
	w.cache = make_cache(device, initialData, size, externallySynced);
	retro_atomic_store_release_int(&w.stop, 0);
	retro_atomic_store_relaxed_int(&w.requested, 0);
	retro_atomic_store_relaxed_int(&w.answered, 0);
	w.syncBuilt = 0;
#ifndef TARGET_NO_THREADS
	w.thread = sthread_create(worker_loop, NULL);
	if (w.thread == NULL)
		WARN_LOG(RENDERER, "Vulkan: no pipeline thread; polygons are drawn with the general pipelines\n");
#endif
}

void vk_pso_worker_stop(VkPipelineCache into)
{
	/* (what is left of the batch is not to be made any more) */
	vk_pso_worker_end_batch(NULL);
#ifndef TARGET_NO_THREADS
	if (w.thread != NULL)
	{
		retro_atomic_fetch_add_seq_cst_int(&w.stop, 1);
		notify(&w.work);
		sthread_join(w.thread);
		w.thread = NULL;
	}
	if (w.events)
	{
		w.events = false;
		retro_eventcount_free(&w.work);
		retro_eventcount_free(&w.idle);
	}
#endif
	if (w.cache != VK_NULL_HANDLE)
	{
		vk_pso_worker_merge_into(into);
		vkDestroyPipelineCache(w.device, w.cache, NULL);
		w.cache = VK_NULL_HANDLE;
	}
	free(w.batch);
	w.batch = NULL;
	w.keep = NULL;
	NOTICE_LOG(RENDERER, "Vulkan pipelines: %u made by the pipeline thread, %u by a draw that had to wait\n",
			vk_pso_worker_built(), w.syncBuilt);
}

bool vk_pso_worker_running(void)
{
	return w.thread != NULL;
}

void vk_pso_worker_merge_into(VkPipelineCache into)
{
	if (w.cache != VK_NULL_HANDLE && into != VK_NULL_HANDLE)
	{
		VkPipelineCache src = w.cache;
		vkMergePipelineCaches(w.device, into, 1, &src);
	}
}

static void ask(PsoSlot *slot, retro_atomic_ptr_t *list)
{
	slot->gen = vk_pso_builder_generation(slot->owner);
	retro_atomic_store_relaxed_int(&slot->state, PSO_QUEUED);
	retro_atomic_store_release_int(&w.requested, retro_atomic_load_relaxed_int(&w.requested) + 1);
	list_push(list, slot);
	notify(&w.work);
}

void vk_pso_worker_request(PsoSlot *slot)
{
	ask(slot, &w.urgent);
}

void vk_pso_worker_prefetch(PsoSlot *slot)
{
	ask(slot, &w.background);
}

void vk_pso_worker_set_draw_cache(VkPipelineCache cache)
{
	w.drawCache = cache;
}

void vk_pso_worker_build(PsoSlot *slot, VkPipelineCache cache)
{
	slot->pipeline = slot->owner->build(slot->owner, slot->key, cache);
	retro_atomic_store_release_int(&slot->state, slot->pipeline != VK_NULL_HANDLE ? PSO_READY : PSO_FAILED);
}

void vk_pso_worker_build_sync(PsoSlot *slot, VkPipelineCache cache)
{
	w.syncBuilt++;
	vk_pso_worker_build(slot, cache);
}

u32 vk_pso_worker_built(void)
{
	return (u32)retro_atomic_load_relaxed_int(&w.built);
}

u32 vk_pso_worker_sync_built(void)
{
	return w.syncBuilt;
}

static bool all_answered(void)
{
	return retro_atomic_load_acquire_int(&w.answered) == retro_atomic_load_relaxed_int(&w.requested);
}

static bool batch_left(void)
{
	return retro_atomic_load_seq_cst_int(&w.batchInside) == 0;
}

static bool batch_waiting(void)
{
	return retro_atomic_load_acquire_int(&w.batchNext) < retro_atomic_load_acquire_int(&w.batchSize);
}

static bool not_building(void)
{
	return retro_atomic_load_acquire_int(&w.waitingFor->state) != PSO_BUILDING;
}

static bool batch_made(void)
{
	const int size = retro_atomic_load_acquire_int(&w.batchSize);
	int i;
	for (i = 0; i < size; i++)
	{
		const int state = retro_atomic_load_acquire_int(&w.batch[i]->state);
		if (state == PSO_QUEUED || state == PSO_BUILDING)
			return false;
	}
	return true;
}

/* parked on the idle eventcount until @done */
static void wait_for(bool (*done)(void))
{
	if (!w.events)
		return;
	for (;;)
	{
		int key;
		if (done())
			return;
		key = retro_eventcount_prepare_wait(&w.idle);
		if (done())
		{
			retro_eventcount_cancel_wait(&w.idle);
			return;
		}
		retro_eventcount_commit_wait(&w.idle, key);
	}
}

void vk_pso_worker_wait_idle(void)
{
	if (w.thread != NULL)
		wait_for(all_answered);
}

void vk_pso_worker_wait_batch(void)
{
	if (w.thread != NULL && w.batch != NULL && retro_atomic_load_relaxed_int(&w.batchActive) != 0)
		wait_for(batch_made);
}

bool vk_pso_worker_batch_ready(void)
{
	return w.thread == NULL || w.batch == NULL || batch_made();
}

/* the next entry of the batch nobody has taken, or -1 */
static int claim_index(void)
{
	for (;;)
	{
		const int i = retro_atomic_load_acquire_int(&w.batchNext);
		if (i >= retro_atomic_load_acquire_int(&w.batchSize))
			return -1;
		if (retro_atomic_cas_int(&w.batchNext, i, i + 1))
			return i;
	}
}

void vk_pso_worker_build_soon(PsoSlot *const *slots, u32 n, VkPipelineCache cache)
{
	int size;
	u32 i;
	if (n == 0)
		return;
	if (!vk_pso_worker_running())
	{
		for (i = 0; i < n; i++)
			vk_pso_worker_build(slots[i], cache);
		return;
	}
	if (w.batch == NULL)
	{
		w.batch = (PsoSlot **)malloc(2 * BATCH_CAPACITY * sizeof(PsoSlot *));
		if (w.batch == NULL)
			abort();
		w.keep = w.batch + BATCH_CAPACITY;
	}
	size = retro_atomic_load_relaxed_int(&w.batchSize);
	for (i = 0; i < n; i++)
	{
		if (size >= BATCH_CAPACITY)
		{
			/* (more than the batch holds: made now, as all of them were once) */
			vk_pso_worker_build(slots[i], cache);
			continue;
		}
		retro_atomic_store_relaxed_int(&slots[i]->state, PSO_QUEUED);
		w.batch[size++] = slots[i];
	}
	retro_atomic_store_release_int(&w.batchSize, size);
	if (retro_atomic_load_relaxed_int(&w.batchActive) == 0)
		retro_atomic_fetch_add_seq_cst_int(&w.batchActive, 1);
	notify(&w.work);
}

VkPipeline vk_pso_worker_need(PsoSlot *slot)
{
	for (;;)
	{
		const int state = retro_atomic_load_acquire_int(&slot->state);
		if (state == PSO_READY)
			return slot->pipeline;
		if (state == PSO_QUEUED)
		{
			if (!retro_atomic_cas_int(&slot->state, PSO_QUEUED, PSO_BUILDING))
				continue;
			w.syncBuilt++;
			slot->pipeline = slot->owner->build(slot->owner, slot->key, w.drawCache);
			retro_atomic_store_release_int(&slot->state, slot->pipeline != VK_NULL_HANDLE ? PSO_READY : PSO_FAILED);
			return slot->pipeline;
		}
		if (state != PSO_BUILDING)
			return VK_NULL_HANDLE;
		/* the worker is making it */
		w.waitingFor = slot;
		wait_for(not_building);
	}
}

void vk_pso_worker_end_batch(const vk_pso_builder *owner)
{
	u32 kept = 0;
	if (w.batch == NULL || retro_atomic_load_relaxed_int(&w.batchActive) == 0)
		return;
	/* what nobody has taken: @owner's dropped, the others' kept to be given again */
	for (;;)
	{
		PsoSlot *slot;
		const int i = claim_index();
		if (i < 0)
			break;
		slot = w.batch[i];
		if (owner == NULL || slot->owner == owner)
			retro_atomic_cas_int(&slot->state, PSO_QUEUED, PSO_FAILED);
		else if (retro_atomic_load_acquire_int(&slot->state) == PSO_QUEUED)
			w.keep[kept++] = slot;
	}
	/* the worker out of the batch, with what it was making made */
	retro_atomic_fetch_add_seq_cst_int(&w.batchActive, -1);
	notify(&w.work);
	wait_for(batch_left);
	retro_atomic_store_relaxed_int(&w.batchNext, 0);
	retro_atomic_store_release_int(&w.batchSize, 0);
	if (kept != 0)
		vk_pso_worker_build_soon(w.keep, kept, w.drawCache);
}

/* the worker's part of the batch: whether it made any */
static bool help_batch(void)
{
	bool any = false;
	if (retro_atomic_load_acquire_int(&w.batchActive) == 0)
		return false;
	retro_atomic_fetch_add_seq_cst_int(&w.batchInside, 1);
	if (retro_atomic_load_seq_cst_int(&w.batchActive) != 0)
	{
		for (;;)
		{
			PsoSlot *slot;
			const int i = claim_index();
			if (i < 0)
				break;
			slot = w.batch[i];
			/* (a draw may have taken it already) */
			if (retro_atomic_cas_int(&slot->state, PSO_QUEUED, PSO_BUILDING))
			{
				vk_pso_worker_build(slot, w.cache);
				retro_atomic_fetch_add_int(&w.built, 1);
				any = true;
			}
			notify(&w.idle);
		}
	}
	retro_atomic_fetch_add_seq_cst_int(&w.batchInside, -1);
	notify(&w.idle);
	return any;
}

static void append(PsoSlot *list, PsoSlot **head, PsoSlot **tail)
{
	if (list == NULL)
		return;
	if (*tail != NULL)
		(*tail)->next = list;
	else
		*head = list;
	*tail = list;
	while ((*tail)->next != NULL)
		*tail = (*tail)->next;
}

static void worker_loop(void *arg)
{
#ifndef TARGET_NO_THREADS
	/* what is to be made, oldest first: the draws' before the rest */
	PsoSlot *urgentHead = NULL, *urgentTail = NULL;
	PsoSlot *bgHead = NULL, *bgTail = NULL;
	(void)arg;
	sthread_setname("vk pipelines");
	/* Below the threads a frame waits for: what it makes is never waited
	 * for (pso.h), and where there are fewer processors than busy threads
	 * the emulation and the drawing go first */
#if defined(__linux__)
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
	for (;;)
	{
		PsoSlot *slot;
		if (help_batch())
			continue;
		append(list_take_all(&w.urgent), &urgentHead, &urgentTail);
		append(list_take_all(&w.background), &bgHead, &bgTail);
		slot = urgentHead;
		if (slot != NULL)
		{
			urgentHead = slot->next;
			if (urgentHead == NULL)
				urgentTail = NULL;
		}
		else if ((slot = bgHead) != NULL)
		{
			bgHead = slot->next;
			if (bgHead == NULL)
				bgTail = NULL;
		}
		if (slot == NULL)
		{
			int key;
			PsoSlot *u, *b;
			if (retro_atomic_load_acquire_int(&w.stop) != 0)
				return;
			key = retro_eventcount_prepare_wait(&w.work);
			u = list_take_all(&w.urgent);
			b = list_take_all(&w.background);
			if (u != NULL || b != NULL || retro_atomic_load_acquire_int(&w.stop) != 0
					|| (retro_atomic_load_acquire_int(&w.batchActive) != 0 && batch_waiting()))
			{
				retro_eventcount_cancel_wait(&w.work);
				append(u, &urgentHead, &urgentTail);
				append(b, &bgHead, &bgTail);
			}
			else
				retro_eventcount_commit_wait(&w.work, key);
			continue;
		}
		slot->next = NULL;
		/* (asked for before its manager started again: dropped) */
		if (slot->gen == vk_pso_builder_generation(slot->owner) && retro_atomic_load_acquire_int(&w.stop) == 0)
		{
			vk_pso_worker_build(slot, w.cache);
			retro_atomic_fetch_add_int(&w.built, 1);
		}
		else
			retro_atomic_store_release_int(&slot->state, PSO_FAILED);
		retro_atomic_fetch_add_int(&w.answered, 1);
		notify(&w.idle);
	}
#else
	(void)arg;
#endif
}
