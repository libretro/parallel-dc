/* A ring of command pools, one a frame of the frontend's swapchain, each
 * with its fence. A frame's place in the ring: its command buffers, made
 * from its pool as they are first asked for and used again every time
 * round (the pool reset frees what they recorded, not them; destroying the
 * pool frees them), and what is to be destroyed once its fence has
 * signaled. Memory only ever grown (grow_buf.h). */
#ifndef VK_CMD_POOL_H
#define VK_CMD_POOL_H

#include "vk_context.h"
#include "rend/grow_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_cmd_deferred
{
	void *object;
	void (*destroy)(void *object);	/* none: a descriptor set, given back to its pool */
	VkDescriptorSet set;
	VkDescriptorPool setPool;
} vk_cmd_deferred;

typedef struct vk_cmd_slot
{
	VkCommandPool pool;
	grow_buf_t buffers;		/* VkCommandBuffer */
	u32 made;
	u32 used;
	grow_buf_t deferred;	/* vk_cmd_deferred */
	u32 deferredCount;
} vk_cmd_slot;

typedef struct vk_cmd_pool
{
	vk_cmd_slot *slots;
	VkFence *fences;	/* a slot's each, side by side for vk_cmd_pool_wait_all() */
	u32 slotCount;
	int index;
	u64 frames;			/* how many have begun: the one being recorded is the last of them */
} vk_cmd_pool;

/* As many slots as the swapchain has images, made or taken down to that.
 * False if one could not be made. */
bool vk_cmd_pool_init(vk_cmd_pool *p);
void vk_cmd_pool_term(vk_cmd_pool *p);
/* the next slot, once its fence has signaled: what it deferred destroyed */
void vk_cmd_pool_begin_frame(vk_cmd_pool *p);
/* its command buffers submitted, as they are: nothing copied or allocated */
void vk_cmd_pool_end_frame(vk_cmd_pool *p);
/* a command buffer of the slot's; VK_NULL_HANDLE if none could be made */
VkCommandBuffer vk_cmd_pool_allocate(vk_cmd_pool *p);
/* Waits until the GPU is done with everything submitted through @p. For
 * before things those frames use are destroyed outright: when the renderer
 * is taken down or resized. The frontend's own wait (wait_sync_index) is
 * for the frontend's frames, not these.
 *
 * It gives up on the fences after a second rather than hang on a frame
 * that was begun and never submitted, and waits for the queue instead: a
 * frame that takes longer than that on the GPU is still being drawn, and
 * what it uses is not to be destroyed under it. */
void vk_cmd_pool_wait_all(vk_cmd_pool *p);
/* Whether the GPU is done with what was last submitted for @slot: its
 * fence, polled */
bool vk_cmd_pool_is_done(const vk_cmd_pool *p, int slot);

static INLINE VkFence vk_cmd_pool_fence(const vk_cmd_pool *p)
{
	return p->fences[p->index];
}

/* Retire a GPU resource so it is destroyed (@destroy(@object)) once the
 * current frame's fence has signaled, rather than stalling the whole device
 * with WaitIdle. Used when an attachment must be recreated (grown) while
 * previous frames may still reference the old one. Destroyed in
 * vk_cmd_pool_begin_frame() when this slot recurs. */
void vk_cmd_pool_defer(vk_cmd_pool *p, void *object, void (*destroy)(void *object));
/* A descriptor set retired the same way, given back to @pool: nothing
 * allocated to hold it (the list keeps its room) */
void vk_cmd_pool_defer_set(vk_cmd_pool *p, VkDescriptorSet set, VkDescriptorPool pool);

#ifdef __cplusplus
}
#endif

#endif
