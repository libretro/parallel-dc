/* See vk_gpu_timer.h. */
#include <stdlib.h>
#include <string.h>
#include "vk_gpu_timer.h"

#define MAX_TIMES (1u << 16)

void vk_gpu_timer_init(vk_gpu_timer *t, int slots, const char *what)
{
	VkQueueFamilyProperties *families;
	VkPhysicalDeviceProperties props;
	VkQueryPoolCreateInfo info;
	uint32_t familyCount = 0;
	const u32 family = vkc.iface->queue_index;
	u32 bits = 0;

	vk_gpu_timer_term(t);
	t->what = what;
	vkGetPhysicalDeviceQueueFamilyProperties(vkc.gpu, &familyCount, NULL);
	families = (VkQueueFamilyProperties *)malloc((familyCount + 1) * sizeof(*families));
	if (families == NULL)
		return;
	vkGetPhysicalDeviceQueueFamilyProperties(vkc.gpu, &familyCount, families);
	if (family < familyCount)
		bits = families[family].timestampValidBits;
	free(families);
	if (bits == 0 || slots <= 0)
		return;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = 2 * (u32)slots;
	t->written = (bool *)calloc((size_t)slots, sizeof(bool));
	t->times = (float *)malloc(MAX_TIMES * sizeof(float));
	if (t->written == NULL || t->times == NULL || vkCreateQueryPool(vkc.device, &info, NULL, &t->queries) != VK_SUCCESS)
	{
		t->queries = VK_NULL_HANDLE;
		vk_gpu_timer_term(t);
		return;
	}
	vkGetPhysicalDeviceProperties(vkc.gpu, &props);
	t->period = props.limits.timestampPeriod;
	t->mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
	t->slots = slots;
	t->count = 0;
}

void vk_gpu_timer_begin(vk_gpu_timer *t, VkCommandBuffer cmd, int slot)
{
	if (t->queries == VK_NULL_HANDLE || slot < 0 || slot >= t->slots)
		return;
	if (t->written[slot])
	{
		u64 ts[2];
		if (vkGetQueryPoolResults(vkc.device, t->queries, 2 * (u32)slot, 2, sizeof(ts), ts, sizeof(u64),
				VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t->count < MAX_TIMES)
			t->times[t->count++] = (float)((double)((ts[1] - ts[0]) & t->mask) * t->period / 1e6);
	}
	vkCmdResetQueryPool(cmd, t->queries, 2 * (u32)slot, 2);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t->queries, 2 * (u32)slot);
	t->written[slot] = true;
}

void vk_gpu_timer_end(vk_gpu_timer *t, VkCommandBuffer cmd, int slot)
{
	if (t->queries == VK_NULL_HANDLE || slot < 0 || slot >= t->slots)
		return;
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t->queries, 2 * (u32)slot + 1);
}

static int float_order(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

void vk_gpu_timer_term(vk_gpu_timer *t)
{
	if (t->count != 0)
	{
		double sum = 0;
		u32 i, p99 = (t->count * 99) / 100;
		qsort(t->times, t->count, sizeof(float), float_order);
		for (i = 0; i < t->count; i++)
			sum += t->times[i];
		if (p99 >= t->count)
			p99 = t->count - 1;
		NOTICE_LOG(RENDERER, "Vulkan %s: GPU time of %u frames: mean %.3f ms, 99th percentile %.3f ms, longest %.3f ms\n", t->what,
				t->count, sum / t->count, t->times[p99], t->times[t->count - 1]);
	}
	t->count = 0;
	free(t->written);
	free(t->times);
	t->written = NULL;
	t->times = NULL;
	t->slots = 0;
	if (t->queries != VK_NULL_HANDLE)
		vkDestroyQueryPool(vkc.device, t->queries, NULL);
	t->queries = VK_NULL_HANDLE;
}
