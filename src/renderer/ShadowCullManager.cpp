#include "Container/renderer/ShadowCullManager.h"

#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>

namespace container::renderer {

using container::gpu::GpuDrawIndexedIndirectCommand;
using container::gpu::ShadowCullData;
using container::gpu::ShadowCullPushConstants;

ShadowCullManager::ShadowCullManager(
	std::shared_ptr<container::gpu::VulkanDevice> device,
	container::gpu::AllocationManager&            allocationManager,
	container::gpu::PipelineManager&              pipelineManager)
	: device_(std::move(device))
	, allocationManager_(allocationManager)
	, pipelineManager_(pipelineManager) {
}

ShadowCullManager::~ShadowCullManager() {
  if (inputDrawBuffer_.buffer != VK_NULL_HANDLE)
	allocationManager_.destroyBuffer(inputDrawBuffer_);
	if (ownedShadowCullUbo_.buffer != VK_NULL_HANDLE)
	allocationManager_.destroyBuffer(ownedShadowCullUbo_);
  for (auto& buffer : indirectDrawBuffers_) {
	if (buffer.buffer != VK_NULL_HANDLE)
	  allocationManager_.destroyBuffer(buffer);
  }
  for (auto& buffer : drawCountBuffers_) {
	if (buffer.buffer != VK_NULL_HANDLE)
	  allocationManager_.destroyBuffer(buffer);
  }

  if (shadowCullPipeline_ != VK_NULL_HANDLE)
	pipelineManager_.destroyPipeline(shadowCullPipeline_);
  if (shadowCullPipelineLayout_ != VK_NULL_HANDLE)
	pipelineManager_.destroyPipelineLayout(shadowCullPipelineLayout_);
  if (shadowCullPool_ != VK_NULL_HANDLE)
	pipelineManager_.destroyDescriptorPool(shadowCullPool_);
  if (shadowCullSetLayout_ != VK_NULL_HANDLE)
	pipelineManager_.destroyDescriptorSetLayout(shadowCullSetLayout_);
}

bool ShadowCullManager::isReady() const {
  return shadowCullPipeline_ != VK_NULL_HANDLE &&
         device_->enabledFeatures().drawIndirectFirstInstance == VK_TRUE;
}

void ShadowCullManager::createResources(const std::filesystem::path& shaderDir,
										uint32_t descriptorSetCount) {
  if (shadowCullSetLayout_ == VK_NULL_HANDLE) {
	const std::array<VkDescriptorSetLayoutBinding, 5> bindings{{
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	}};
	shadowCullSetLayout_ = pipelineManager_.createDescriptorSetLayout(
		{bindings.begin(), bindings.end()}, {0});
  }

	if (ownedShadowCullUbo_.buffer == VK_NULL_HANDLE) {
	ownedShadowCullUbo_ = allocationManager_.createBuffer(
		sizeof(ShadowCullData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT);
	shadowCullUboSize_ = sizeof(ShadowCullData);
  }

	recreatePerFrameResources(descriptorSetCount);
	createShadowCullPipeline(shaderDir);
	for (uint32_t i = 0; i < shadowCullBuffers_.size(); ++i) {
	  writeDescriptorSets(i);
  }
}

void ShadowCullManager::recreatePerFrameResources(uint32_t descriptorSetCount) {
	const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
	if (shadowCullPool_ != VK_NULL_HANDLE) {
	  pipelineManager_.destroyDescriptorPool(shadowCullPool_);
	}
	shadowCullPool_ = pipelineManager_.createDescriptorPool(
		{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * container::gpu::kShadowCascadeCount},
		 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * container::gpu::kShadowCascadeCount * 4}},
		setCount * container::gpu::kShadowCascadeCount, 0);

	shadowCullSets_.assign(setCount * container::gpu::kShadowCascadeCount, VK_NULL_HANDLE);
	shadowCullBuffers_.assign(setCount, ownedShadowCullUbo_.buffer);
	std::vector<VkDescriptorSetLayout> layouts(shadowCullSets_.size(), shadowCullSetLayout_);
	VkDescriptorSetAllocateInfo allocInfo{
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocInfo.descriptorPool     = shadowCullPool_;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
	allocInfo.pSetLayouts        = layouts.data();
	if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
							 shadowCullSets_.data()) != VK_SUCCESS) {
	  throw std::runtime_error("failed to allocate shadow cull descriptor sets");
	}
}

bool ShadowCullManager::ensureBufferCapacity(uint32_t maxDrawCount) {
  if (maxDrawCount <= maxDrawCount_ && inputDrawBuffer_.buffer != VK_NULL_HANDLE) {
	return false;
  }

  const uint32_t capacity = std::max(maxDrawCount, 64u);

  if (inputDrawBuffer_.buffer != VK_NULL_HANDLE)
	allocationManager_.destroyBuffer(inputDrawBuffer_);
  for (auto& buffer : indirectDrawBuffers_) {
	if (buffer.buffer != VK_NULL_HANDLE)
	  allocationManager_.destroyBuffer(buffer);
  }
  for (auto& buffer : drawCountBuffers_) {
	if (buffer.buffer != VK_NULL_HANDLE)
	  allocationManager_.destroyBuffer(buffer);
  }

  inputDrawBuffer_ = allocationManager_.createBuffer(
	  sizeof(GpuDrawIndexedIndirectCommand) * capacity,
	  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	  VMA_MEMORY_USAGE_AUTO,
	  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		  VMA_ALLOCATION_CREATE_MAPPED_BIT);

  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
	indirectDrawBuffers_[i] = allocationManager_.createBuffer(
		sizeof(GpuDrawIndexedIndirectCommand) * capacity,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	drawCountBuffers_[i] = allocationManager_.createBuffer(
		sizeof(uint32_t),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
  }

  maxDrawCount_ = capacity;
	for (uint32_t i = 0; i < shadowCullBuffers_.size(); ++i) {
	writeDescriptorSets(i);
  }
  return true;
}

void ShadowCullManager::uploadDrawCommands(const std::vector<DrawCommand>& commands) {
  if (inputDrawBuffer_.buffer == VK_NULL_HANDLE || commands.empty()) return;

  const uint32_t count =
	  std::min(static_cast<uint32_t>(commands.size()), maxDrawCount_);
  std::vector<GpuDrawIndexedIndirectCommand> gpuCmds(count);
  for (uint32_t i = 0; i < count; ++i) {
	gpuCmds[i].indexCount    = commands[i].indexCount;
	gpuCmds[i].instanceCount = 1;
	gpuCmds[i].firstIndex    = commands[i].firstIndex;
	gpuCmds[i].vertexOffset  = 0;
	gpuCmds[i].firstInstance = commands[i].objectIndex;
  }

  SceneController::writeToBuffer(allocationManager_, inputDrawBuffer_,
								 gpuCmds.data(),
								 sizeof(GpuDrawIndexedIndirectCommand) * count);
}

void ShadowCullManager::updateObjectSsboDescriptor(
	VkBuffer objectBuffer,
	VkDeviceSize objectBufferSize) {
	objectSsboBuffer_  = objectBuffer;
	objectSsboSize_    = objectBufferSize;
	objectCount_       = static_cast<uint32_t>(objectBufferSize / sizeof(container::gpu::ObjectData));
	for (uint32_t i = 0; i < shadowCullBuffers_.size(); ++i) {
	writeDescriptorSets(i);
  }
}

void ShadowCullManager::updateShadowCullDescriptor(
	uint32_t imageIndex,
	VkBuffer shadowCullBuffer,
	VkDeviceSize shadowCullBufferSize) {
	if (imageIndex >= shadowCullBuffers_.size()) return;
	shadowCullBuffers_[imageIndex] = shadowCullBuffer;
	shadowCullUboSize_    = shadowCullBufferSize;
  writeDescriptorSets(imageIndex);
}

void ShadowCullManager::dispatchCascadeCull(VkCommandBuffer cmd,
									uint32_t imageIndex,
											uint32_t cascadeIndex,
											uint32_t drawCount,
											uint32_t outputOffset) {
	if (shadowCullPipeline_ == VK_NULL_HANDLE ||
	  imageIndex >= shadowCullBuffers_.size() ||
	  cascadeIndex >= container::gpu::kShadowCascadeCount ||
	  shadowCullBuffers_[imageIndex] == VK_NULL_HANDLE ||
	  objectSsboBuffer_ == VK_NULL_HANDLE ||
	  inputDrawBuffer_.buffer == VK_NULL_HANDLE ||
	  indirectDrawBuffers_[cascadeIndex].buffer == VK_NULL_HANDLE ||
	  drawCountBuffers_[cascadeIndex].buffer == VK_NULL_HANDLE) {
	return;
  }

	vkCmdFillBuffer(cmd, drawCountBuffers_[cascadeIndex].buffer, 0,
					 sizeof(uint32_t), 0);

	VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	fillBarrier.dstAccessMask =
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
					 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					 0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

	const size_t setIndex = descriptorSetIndex(imageIndex, cascadeIndex);
	if (setIndex >= shadowCullSets_.size() ||
		shadowCullSets_[setIndex] == VK_NULL_HANDLE) {
	  return;
	}
	const VkDescriptorSet descriptorSet = shadowCullSets_[setIndex];
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadowCullPipeline_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
					shadowCullPipelineLayout_, 0, 1,
					&descriptorSet, 0, nullptr);

	ShadowCullPushConstants pc{};
	pc.drawCount    = drawCount;
	pc.cascadeIndex = cascadeIndex;
	pc.outputOffset = outputOffset;
	pc.objectCount  = objectCount_;
	vkCmdPushConstants(cmd, shadowCullPipelineLayout_,
				   VK_SHADER_STAGE_COMPUTE_BIT, 0,
				   sizeof(ShadowCullPushConstants), &pc);

	const uint32_t groupCount = (drawCount + 63u) / 64u;
	if (groupCount > 0u) {
	  vkCmdDispatch(cmd, groupCount, 1, 1);
	}

	VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
					  VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd,
				 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
					 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
					 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ShadowCullManager::createShadowCullPipeline(
	const std::filesystem::path& shaderDir) {
	auto compPath = shaderDir / "spv_shaders" / "shadow_cull.comp.spv";
	if (!std::filesystem::exists(compPath)) return;

	VkPushConstantRange pcRange{};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.size       = sizeof(ShadowCullPushConstants);

	if (shadowCullSetLayout_ == VK_NULL_HANDLE) return;

	if (shadowCullPipelineLayout_ == VK_NULL_HANDLE) {
	  shadowCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
		  {shadowCullSetLayout_}, {pcRange});
	}

	if (shadowCullPipelineLayout_ == VK_NULL_HANDLE) return;

	const auto spvData = container::util::readFile(compPath);
	VkShaderModule compModule =
		container::gpu::createShaderModule(device_->device(), spvData);

	VkPipelineShaderStageCreateInfo stage{};
	stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = compModule;
	stage.pName  = "computeMain";

	VkComputePipelineCreateInfo ci{};
	ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	ci.stage  = stage;
	ci.layout = shadowCullPipelineLayout_;

	shadowCullPipeline_ =
		pipelineManager_.createComputePipeline(ci, "shadow_cull");
	vkDestroyShaderModule(device_->device(), compModule, nullptr);
}

size_t ShadowCullManager::descriptorSetIndex(uint32_t imageIndex,
											 uint32_t cascadeIndex) const {
  return static_cast<size_t>(imageIndex) * container::gpu::kShadowCascadeCount +
		 static_cast<size_t>(cascadeIndex);
}

void ShadowCullManager::writeDescriptorSets(uint32_t imageIndex) {
  if (imageIndex >= shadowCullBuffers_.size()) return;
  for (uint32_t cascadeIndex = 0;
	   cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
	const size_t setIndex = descriptorSetIndex(imageIndex, cascadeIndex);
	if (setIndex >= shadowCullSets_.size() ||
		shadowCullSets_[setIndex] == VK_NULL_HANDLE) continue;

  VkDescriptorBufferInfo shadowCullInfo{
		shadowCullBuffers_[imageIndex], 0,
	  shadowCullUboSize_ > 0 ? shadowCullUboSize_ : sizeof(ShadowCullData)};
  VkDescriptorBufferInfo objectInfo{
		objectSsboBuffer_, 0, objectSsboSize_};
  VkDescriptorBufferInfo inputDrawInfo{
	  inputDrawBuffer_.buffer, 0,
	  sizeof(GpuDrawIndexedIndirectCommand) * std::max(maxDrawCount_, 1u)};
  VkDescriptorBufferInfo outputDrawInfo{
	  indirectDrawBuffers_[cascadeIndex].buffer, 0,
	  sizeof(GpuDrawIndexedIndirectCommand) * std::max(maxDrawCount_, 1u)};
  VkDescriptorBufferInfo drawCountInfo{
	  drawCountBuffers_[cascadeIndex].buffer, 0, sizeof(uint32_t)};

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(5);

  const auto addBufferWrite = [&](uint32_t binding,
                                  VkDescriptorType descriptorType,
                                  const VkDescriptorBufferInfo& bufferInfo) {
    if (bufferInfo.buffer == VK_NULL_HANDLE || bufferInfo.range == 0) return;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet          = shadowCullSets_[setIndex];
    write.dstBinding      = binding;
    write.descriptorCount = 1;
    write.descriptorType  = descriptorType;
    write.pBufferInfo     = &bufferInfo;
    writes.push_back(write);
  };

  addBufferWrite(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, shadowCullInfo);
  addBufferWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, objectInfo);
  addBufferWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, inputDrawInfo);
  addBufferWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, outputDrawInfo);
  addBufferWrite(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawCountInfo);

  if (!writes.empty()) {
    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }
  }
}

}  // namespace container::renderer
