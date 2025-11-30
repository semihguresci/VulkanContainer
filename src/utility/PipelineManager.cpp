#include <Container/utility/PipelineManager.h>

#include <stdexcept>

namespace utility::pipeline {

PipelineManager::PipelineManager(VkDevice device) : device_(device) {}

PipelineManager::~PipelineManager() { destroyManagedResources(); }

VkDescriptorSetLayout PipelineManager::createDescriptorSetLayout(
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    const std::vector<VkDescriptorBindingFlags>& bindingFlags,
    VkDescriptorSetLayoutCreateFlags flags, const void* next) {
  VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
  bindingFlagsInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  bindingFlagsInfo.bindingCount =
      static_cast<uint32_t>(bindingFlags.size());
  bindingFlagsInfo.pBindingFlags = bindingFlags.data();

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();
  layoutInfo.flags = flags;
  layoutInfo.pNext = next ? next : &bindingFlagsInfo;

  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create descriptor set layout");
  }

  descriptorSetLayouts_.push_back(setLayout);
  return setLayout;
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
  if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create descriptor pool");
  }

  descriptorPools_.push_back(pool);
  return pool;
}

VkPipelineLayout PipelineManager::createPipelineLayout(
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::vector<VkPushConstantRange>& pushConstantRanges,
    const void* next) {
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount =
      static_cast<uint32_t>(setLayouts.size());
  pipelineLayoutInfo.pSetLayouts = setLayouts.data();
  pipelineLayoutInfo.pushConstantRangeCount =
      static_cast<uint32_t>(pushConstantRanges.size());
  pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();
  pipelineLayoutInfo.pNext = next;

  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &layout) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline layout");
  }

  pipelineLayouts_.push_back(layout);
  return layout;
}

VkPipelineCache PipelineManager::getOrCreatePipelineCache(
    const std::string& cacheKey, const VkPipelineCacheCreateInfo* createInfo) {
  if (auto it = pipelineCaches_.find(cacheKey); it != pipelineCaches_.end()) {
    return it->second;
  }

  VkPipelineCacheCreateInfo cacheInfo{};
  if (createInfo != nullptr) {
    cacheInfo = *createInfo;
  } else {
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.pNext = nullptr;
    cacheInfo.flags = 0;
    cacheInfo.initialDataSize = 0;
    cacheInfo.pInitialData = nullptr;
  }

  VkPipelineCache cache = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &cache) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline cache");
  }

  pipelineCaches_.emplace(cacheKey, cache);
  return cache;
}

VkPipeline PipelineManager::createGraphicsPipeline(
    const VkGraphicsPipelineCreateInfo& pipelineInfo,
    const std::string& cacheKey) {
  VkGraphicsPipelineCreateInfo pipelineCreateInfo = pipelineInfo;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineCache cache = getOrCreatePipelineCache(cacheKey, nullptr);
  if (vkCreateGraphicsPipelines(device_, cache, 1, &pipelineCreateInfo, nullptr,
                                &pipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create graphics pipeline");
  }

  pipelines_.push_back(pipeline);
  return pipeline;
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

}  // namespace utility::pipeline

