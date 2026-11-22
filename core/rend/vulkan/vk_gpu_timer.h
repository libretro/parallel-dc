/* The GPU time of the screen's renders, for the log: a timestamp before a
 * frame's commands and one after, read back when the frame's slot of the
 * command pool comes round again - its fence has been waited for by then,
 * so the read never waits, and nothing is read before that. Nothing is
 * allocated after vk_gpu_timer_init(): the times are kept in room made
 * there.
 *
 * At vk_gpu_timer_term(), one line: how many frames, and their mean, 99th percentile
 * and longest GPU time. */

#ifndef VK_GPU_TIMER_H
#define VK_GPU_TIMER_H

#include "vk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_gpu_timer
{
	VkQueryPool queries;
	double period;		/* ns a tick */
	u64 mask;
	bool *written;		/* a slot's each */
	int slots;
	float *times;		/* ms */
	u32 count;
	const char *what;
} vk_gpu_timer;

void vk_gpu_timer_init(vk_gpu_timer *t, int slots, const char *what);
/* Before the first command of the frame in @slot of the command pool,
 * outside a render pass: the time the slot's last frame took is taken */
void vk_gpu_timer_begin(vk_gpu_timer *t, VkCommandBuffer cmd, int slot);
/* ...and after its last */
void vk_gpu_timer_end(vk_gpu_timer *t, VkCommandBuffer cmd, int slot);
void vk_gpu_timer_term(vk_gpu_timer *t);

#ifdef __cplusplus
}
#endif

#endif
