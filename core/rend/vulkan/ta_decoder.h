/* The C++ renderers' view of the GPU-side TA processing
 * (vk_ta_decoder.h), until they are C too */
#pragma once
#include "vulkan.h"
#include "vk_ta_decoder.h"
#include "commandpool.h"

#include <cstddef>

/* vk_ta_gpu_draw, its buffers as vk::Buffer (which is a VkBuffer) */
struct TaGpuDraw
{
	vk::Buffer vertices, indices, modtrig, commands, trDraws, trParams;
	vk::DeviceSize trParamsSize;
	vk::Buffer frame;
	PFN_vkCmdDrawIndexedIndirectCountKHR drawCount;
	vk::DeviceSize shadowCommands;
	vk::Buffer sorted;
	vk::DeviceSize sortedOffset;
	u32 *sortedMap;
};
static_assert(sizeof(TaGpuDraw) == sizeof(vk_ta_gpu_draw), "TaGpuDraw is vk_ta_gpu_draw");
static_assert(offsetof(TaGpuDraw, trParamsSize) == offsetof(vk_ta_gpu_draw, trParamsSize), "TaGpuDraw is vk_ta_gpu_draw");
static_assert(offsetof(TaGpuDraw, frame) == offsetof(vk_ta_gpu_draw, frame), "TaGpuDraw is vk_ta_gpu_draw");
static_assert(offsetof(TaGpuDraw, shadowCommands) == offsetof(vk_ta_gpu_draw, shadowCommands), "TaGpuDraw is vk_ta_gpu_draw");
static_assert(offsetof(TaGpuDraw, sortedMap) == offsetof(vk_ta_gpu_draw, sortedMap), "TaGpuDraw is vk_ta_gpu_draw");

class TaDecoder
{
public:
	TaDecoder() { vk_ta_decoder_construct(&d); }
	TaDecoder(const TaDecoder&) = delete;
	TaDecoder& operator=(const TaDecoder&) = delete;
	~TaDecoder() { Term(); }

	void Setup(CommandPool& pool, bool wanted) { vk_ta_decoder_setup(&d, &pool.p, wanted); }
	void BeginFrame(CommandPool& pool) { vk_ta_decoder_begin_frame(&d, &pool.p); }
	void Term() { vk_ta_decoder_term(&d); }
	bool Ready() const { return d.ready; }
	bool Prepare(TA_context *ctx, u32 flags, bool *rv) { return vk_ta_decoder_prepare(&d, ctx, flags, rv); }
	void Record(TA_context *ctx, CommandPool& pool)
	{
		if (!vk_ta_decoder_record(&d, ctx, &pool.p))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateComputePipelines");
	}
	const TaGpuDraw *Draw() const { return reinterpret_cast<const TaGpuDraw *>(&d.draw); }
	bool HasInstance() const { return d.instance; }
	void WantSort(bool on) { d.sortWanted = on; }
	bool CanSort() const { return d.sortable; }

private:
	vk_ta_decoder d;
};
