/* The chip's write-out for the Vulkan raster renderers: see writeout.h */
#include "writeout.h"
#include "vulkan_context.h"

// (compute_drawer.cpp has the kernels' SPIR-V)
const u32 *vk_compute_kernel(u32 kernel, size_t *size);

// a slot of words a picture: the frame's PVRC_F_WIDTH to PVRC_F_FLAGS, at an offset any device takes
static const u32 SlotBytes = 256;
static_assert(PVRC_F_FLAGS < SlotBytes / 4 && PVRC_F_WIDTH < SlotBytes / 4 && PVRC_F_HEIGHT < SlotBytes / 4, "the pass's words in a slot");

void VulkanWriteOut::Term()
{
	overlayFramebuffers.clear();
	overlayPass.reset();
	sets.clear();
	pool.reset();
	pipeline.reset();
	pipelineLayout.reset();
	setLayout.reset();
	words.reset();
	mapped = nullptr;
}

void VulkanWriteOut::Init(const std::vector<vk::ImageView>& views, vk::Extent2D pictureSize)
{
	Term();
	const u32 count = (u32)views.size();
	if (!Wanted() || count == 0)
		return;
	VulkanContext *context = VulkanContext::Instance();
	vk::Device device = context->GetDevice();
	// (the kernel's bindings: its frame's words, 0, and the picture, 3)
	const vk::DescriptorSetLayoutBinding bindings[] = {
		{ 0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ 3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute },
	};
	setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), 2, bindings));
	pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &setLayout.get()));
	size_t size = 0;
	const u32 *code = vk_compute_kernel(PVRC_K_YFILTER, &size);
	vk::UniqueShaderModule module = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(), size, code));
	const vk::PipelineShaderStageCreateInfo stage(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute, *module, "main");
	pipeline = device.createComputePipelineUnique(context->GetPipelineCache(),
			vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(), stage, *pipelineLayout));

	const vk::DescriptorPoolSize sizes[] = {
		{ vk::DescriptorType::eStorageBuffer, count },
		{ vk::DescriptorType::eStorageImage, count },
	};
	pool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), count, 2, sizes));
	std::vector<vk::DescriptorSetLayout> layouts(count, *setLayout);
	sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*pool, count, layouts.data()));
	words = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)SlotBytes * count, vk::BufferUsageFlagBits::eStorageBuffer));
	mapped = (u32 *)words->MapMemory();
	for (u32 i = 0; i < count; i++)
	{
		const vk::DescriptorBufferInfo info(*words->buffer, (vk::DeviceSize)SlotBytes * i, SlotBytes);
		device.updateDescriptorSets(vk::WriteDescriptorSet(sets[i], 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &info), nullptr);
	}

	/* What goes over the picture: a render pass that loads it as the pass
	 * left it, and leaves it as the frame's render pass does */
	const vk::AttachmentDescription attachment(vk::AttachmentDescriptionFlags(), context->GetColorFormat(), vk::SampleCountFlagBits::e1,
			vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	const vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	const vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, 0, nullptr, 1, &colorReference);
	const vk::SubpassDependency dependencies[] = {
		// (after Record()'s barrier, whose second half it follows on from)
		vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader
				| vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite),
		vk::SubpassDependency(0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
				vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead),
	};
	overlayPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), 1, &attachment, 1, &subpass, 2, dependencies));
	extent = pictureSize;
	for (u32 i = 0; i < count; i++)
		overlayFramebuffers.push_back(device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(), *overlayPass,
				1, &views[i], extent.width, extent.height, 1)));
}

void VulkanWriteOut::BeginOverlay(vk::CommandBuffer cmd, u32 index)
{
	cmd.beginRenderPass(vk::RenderPassBeginInfo(*overlayPass, *overlayFramebuffers[index], vk::Rect2D({ 0, 0 }, extent), 0, nullptr),
			vk::SubpassContents::eInline);
}

void VulkanWriteOut::Record(vk::CommandBuffer cmd, u32 index, vk::Image image, vk::ImageView view, u32 width, u32 height)
{
	if (!pipeline || index >= sets.size())
		return;
	const u32 flags = ComputeFrame::WriteOut(height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	/* (this picture's set and slot: the frame before that drew in it is
	 * done, as the command pool has waited for it) */
	u32 *w = mapped + index * (SlotBytes / 4);
	w[PVRC_F_WIDTH] = width;
	w[PVRC_F_HEIGHT] = height;
	w[PVRC_F_FLAGS] = flags;
	const vk::DescriptorImageInfo info(nullptr, view, vk::ImageLayout::eGeneral);
	VulkanContext::Instance()->GetDevice().updateDescriptorSets(
			vk::WriteDescriptorSet(sets[index], 3, 0, 1, vk::DescriptorType::eStorageImage, &info), nullptr);

	const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
	const vk::ImageMemoryBarrier toGeneral(vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eComputeShader,
			vk::DependencyFlags(), nullptr, nullptr, toGeneral);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, 1, &sets[index], 0, nullptr);
	cmd.dispatch((width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	// (for the frontend and the next frame's drawing over it, and a copy when the context goes)
	const vk::ImageMemoryBarrier toRead(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
			vk::DependencyFlags(), nullptr, nullptr, toRead);
}
