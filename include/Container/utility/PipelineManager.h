#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace utility::pipeline {

class PipelineManager {
 public:
  explicit PipelineManager(VkDevice device);
  ~PipelineManager();

  PipelineManager(const PipelineManager&) = delete;
  PipelineManager& operator=(const PipelineManager&) = delete;
  PipelineManager(PipelineManager&&) = delete;
  PipelineManager& operator=(PipelineManager&&) = delete;

  VkDescriptorSetLayout createDescriptorSetLayout(
      const std::vector<VkDescriptorSetLayoutBinding>& bindings,
      const std::vector<VkDescriptorBindingFlags>& bindingFlags,
      VkDescriptorSetLayoutCreateFlags flags = 0,
      const void* next = nullptr);

  VkDescriptorPool createDescriptorPool(
      const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets,
      VkDescriptorPoolCreateFlags flags = 0);

  VkPipelineLayout createPipelineLayout(
      const std::vector<VkDescriptorSetLayout>& setLayouts,
      const std::vector<VkPushConstantRange>& pushConstantRanges,
      const void* next = nullptr);

  VkPipelineCache getOrCreatePipelineCache(
      const std::string& cacheKey,
      const VkPipelineCacheCreateInfo* createInfo = nullptr);

  VkPipeline createGraphicsPipeline(
      const VkGraphicsPipelineCreateInfo& pipelineInfo,
      const std::string& cacheKey);

  void destroyManagedResources();

 private:
  VkDevice device_{VK_NULL_HANDLE};
  std::unordered_map<std::string, VkPipelineCache> pipelineCaches_{};
  std::vector<VkDescriptorSetLayout> descriptorSetLayouts_{};
  std::vector<VkDescriptorPool> descriptorPools_{};
  std::vector<VkPipelineLayout> pipelineLayouts_{};
  std::vector<VkPipeline> pipelines_{};
};

}  // namespace utility::pipeline

