/* The GPU time of the screen's renders, for the log: a timestamp before a
 * frame's commands and one after, read back when the frame's slot of the
 * command pool comes round again - its fence has been waited for by then,
 * so the read never waits, and nothing is read before that. Nothing is
 * allocated after Init(): the times are kept in room made there.
 *
 * At Term(), one line: how many frames, and their mean, 99th percentile
 * and longest GPU time. */
#pragma once
#include "vulkan_context.h"
#include "vk_gpu_timer.h"

/* The C++ renderers' view of a vk_gpu_timer, until they are C too */
class GpuFrameTimer
{
public:
	GpuFrameTimer() { memset(&t, 0, sizeof(t)); t.what = ""; }
	~GpuFrameTimer() { vk_gpu_timer_term(&t); }
	void Init(int slots, const char *what) { vk_gpu_timer_init(&t, slots, what); }
	void Begin(vk::CommandBuffer cmd, int slot) { vk_gpu_timer_begin(&t, (VkCommandBuffer)cmd, slot); }
	void End(vk::CommandBuffer cmd, int slot) { vk_gpu_timer_end(&t, (VkCommandBuffer)cmd, slot); }
	void Term() { vk_gpu_timer_term(&t); }

private:
	vk_gpu_timer t;
};
