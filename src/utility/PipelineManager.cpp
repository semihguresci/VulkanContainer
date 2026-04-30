#include "Container/utility/PipelineManager.h"

#include <algorithm>
#include <stdexcept>

namespace container::gpu {

PipelineManager::PipelineManager(VkDevice device) : device_(device) {}

PipelineManager::~PipelineManager() { destroyManagedResources(); }

VkDescriptorSetLayout PipelineManager::createDescriptorSetLayout(
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    const std::vector<VkDescriptorBindingFlags>& bindingFlags,
    VkDescriptorSetLayoutCreateFlags flags, const void* next) {
  if (!bindingFlags.empty() && bindingFlags.size() != bindings.size()) {
    throw std::runtime_error(
        "Descriptor binding flags count must match layout binding count");
  }

  VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
  bindingFlagsInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
  bindingFlagsInfo.pBindingFlags = bindingFlags.data();
  bindingFlagsInfo.pNext = next;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();
  layoutInfo.flags = flags;
  layoutInfo.pNext = bindingFlags.empty() ? next : &bindingFlagsInfo;

  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkResult res =
      vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create descriptor set layout");
  }

  descriptorSetLayouts_.push_back(setLayout);
  return setLayout;
}

void PipelineManager::destroyDescriptorSetLayout(
    VkDescriptorSetLayout& layout) {
  if (layout == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(descriptorSetLayouts_, layout);
  if (it != descriptorSetLayouts_.end()) {
    descriptorSetLayouts_.erase(it);
  }

  vkDestroyDescriptorSetLayout(device_, layout, nullptr);
  layout = VK_NULL_HANDLE;
}

VkDescriptorPool PipelineManager::createDescriptorPool(
    const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets,
    VkDescriptorPoolCreateFlags flags) {
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = maxSets;
  poolInfo.flags = flags;

  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkResult res = vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create descriptor pool");
  }

  descriptorPools_.push_back(pool);
  return pool;
}

void PipelineManager::destroyDescriptorPool(VkDescriptorPool& pool) {
  if (pool == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(descriptorPools_, pool);
  if (it != descriptorPools_.end()) {
    descriptorPools_.erase(it);
  }

  vkDestroyDescriptorPool(device_, pool, nullptr);
  pool = VK_NULL_HANDLE;
}

VkPipelineLayout PipelineManager::createPipelineLayout(
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::vector<VkPushConstantRange>& pushConstantRanges,
    const void* next) {
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
  pipelineLayoutInfo.pSetLayouts = setLayouts.data();
  pipelineLayoutInfo.pushConstantRangeCount =
      static_cast<uint32_t>(pushConstantRanges.size());
  pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();
  pipelineLayoutInfo.pNext = next;

  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkResult res =
      vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &layout);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create pipeline layout");
  }

  pipelineLayouts_.push_back(layout);
  return layout;
}

void PipelineManager::destroyPipelineLayout(VkPipelineLayout& layout) {
  if (layout == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(pipelineLayouts_, layout);
  if (it != pipelineLayouts_.end()) {
    pipelineLayouts_.erase(it);
  }

  vkDestroyPipelineLayout(device_, layout, nullptr);
  layout = VK_NULL_HANDLE;
}

VkPipelineCache PipelineManager::getOrCreatePipelineCache(
    const std::string& cacheKey, const VkPipelineCacheCreateInfo* createInfo) {
  if (auto it = pipelineCaches_.find(cacheKey); it != pipelineCaches_.end()) {
    return it->second;
  }

  VkPipelineCacheCreateInfo cacheInfo{};
  cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

  if (createInfo) {
    cacheInfo = *createInfo;
  }

  VkPipelineCache cache = VK_NULL_HANDLE;
  VkResult res = vkCreatePipelineCache(device_, &cacheInfo, nullptr, &cache);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create pipeline cache");
  }

  pipelineCaches_.emplace(cacheKey, cache);
  return cache;
}

VkPipeline PipelineManager::createGraphicsPipeline(
    const VkGraphicsPipelineCreateInfo& pipelineInfo,
    const std::string& cacheKey) {
  VkPipelineCache cache = getOrCreatePipelineCache(cacheKey, nullptr);

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult res = vkCreateGraphicsPipelines(device_, cache, 1, &pipelineInfo,
                                           nullptr, &pipeline);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create graphics pipeline");
  }

  pipelines_.push_back(pipeline);
  return pipeline;
}

VkPipeline PipelineManager::createComputePipeline(
    const VkComputePipelineCreateInfo& pipelineInfo,
    const std::string& cacheKey) {
  VkPipelineCache cache = getOrCreatePipelineCache(cacheKey, nullptr);

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult res = vkCreateComputePipelines(device_, cache, 1, &pipelineInfo,
                                          nullptr, &pipeline);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to create compute pipeline");
  }

  pipelines_.push_back(pipeline);
  return pipeline;
}

void PipelineManager::destroyPipeline(VkPipeline& pipeline) {
  if (pipeline == VK_NULL_HANDLE) return;

  const auto it = std::ranges::find(pipelines_, pipeline);
  if (it != pipelines_.end()) {
    pipelines_.erase(it);
  }

  vkDestroyPipeline(device_, pipeline, nullptr);
  pipeline = VK_NULL_HANDLE;
}

void PipelineManager::destroyManagedResources() {
  for (VkPipeline pipeline : pipelines_) {
    vkDestroyPipeline(device_, pipeline, nullptr);
  }
  pipelines_.clear();

  for (VkPipelineLayout layout : pipelineLayouts_) {
    vkDestroyPipelineLayout(device_, layout, nullptr);
  }
  pipelineLayouts_.clear();

  for (VkDescriptorPool pool : descriptorPools_) {
    vkDestroyDescriptorPool(device_, pool, nullptr);
  }
  descriptorPools_.clear();

  for (VkDescriptorSetLayout layout : descriptorSetLayouts_) {
    vkDestroyDescriptorSetLayout(device_, layout, nullptr);
  }
  descriptorSetLayouts_.clear();

  for (auto& [_, cache] : pipelineCaches_) {
    vkDestroyPipelineCache(device_, cache, nullptr);
  }
  pipelineCaches_.clear();
}

}  // namespace container::gpu
