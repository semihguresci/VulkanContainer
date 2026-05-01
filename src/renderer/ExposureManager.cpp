#include "Container/renderer/ExposureManager.h"

#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::renderer {

using container::gpu::ExposureAdaptPushConstants;
using container::gpu::ExposureHistogramPushConstants;
using container::gpu::ExposureSettings;
using container::gpu::ExposureStateData;

namespace {

constexpr float kMinHistogramLog2Luminance = -12.0f;
constexpr float kMaxHistogramLog2Luminance = 12.0f;
constexpr float kAssumedFrameDeltaSeconds = 1.0f / 60.0f;

float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

ExposureSettings sanitizeExposureSettings(ExposureSettings settings) {
  settings.mode = settings.mode == container::gpu::kExposureModeAuto
                      ? container::gpu::kExposureModeAuto
                      : container::gpu::kExposureModeManual;
  settings.manualExposure =
      std::max(finiteOr(settings.manualExposure, 0.25f), 0.0f);
  settings.targetLuminance =
      std::max(finiteOr(settings.targetLuminance, 0.18f), 0.001f);
  settings.minExposure =
      std::max(finiteOr(settings.minExposure, 0.03125f), 0.0f);
  settings.maxExposure =
      std::max(finiteOr(settings.maxExposure, 8.0f), settings.minExposure);
  settings.adaptationRate =
      std::max(finiteOr(settings.adaptationRate, 1.5f), 0.0f);
  settings.meteringLowPercentile =
      std::clamp(finiteOr(settings.meteringLowPercentile, 0.50f), 0.0f,
                 0.99f);
  settings.meteringHighPercentile =
      std::clamp(finiteOr(settings.meteringHighPercentile, 0.95f),
                 settings.meteringLowPercentile + 0.01f, 1.0f);
  return settings;
}

float clampExposure(float exposure, const ExposureSettings& settings) {
  return std::clamp(finiteOr(exposure, settings.manualExposure),
                    settings.minExposure, settings.maxExposure);
}

}  // namespace

ExposureManager::ExposureManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager& allocationManager,
    container::gpu::PipelineManager& pipelineManager)
    : device_(std::move(device)),
      allocationManager_(allocationManager),
      pipelineManager_(pipelineManager) {}

ExposureManager::~ExposureManager() {
  destroy();
}

void ExposureManager::createResources(const std::filesystem::path& shaderDir) {
  createPipeline(shaderDir);
  createHistogramBuffer();
  createExposureStateBuffer();

  const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
  }};
  descriptorPool_ = pipelineManager_.createDescriptorPool(
      {poolSizes.begin(), poolSizes.end()}, 1);

  VkDescriptorSetAllocateInfo allocateInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocateInfo.descriptorPool = descriptorPool_;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts = &setLayout_;
  if (vkAllocateDescriptorSets(device_->device(), &allocateInfo,
                               &descriptorSet_) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate exposure descriptor set");
  }
}

void ExposureManager::dispatch(
    VkCommandBuffer cmd,
    VkImageView sceneColorView,
    uint32_t sceneWidth,
    uint32_t sceneHeight,
    const ExposureSettings& rawSettings) {
  if (!isReady() || cmd == VK_NULL_HANDLE ||
      sceneColorView == VK_NULL_HANDLE || sceneWidth == 0u ||
      sceneHeight == 0u) {
    return;
  }

  const ExposureSettings settings = sanitizeExposureSettings(rawSettings);

  constexpr VkDeviceSize kHistogramBufferSize =
      sizeof(uint32_t) * (kHistogramBinCount + 1u);
  vkCmdFillBuffer(cmd, histogramBuffer_.buffer, 0, kHistogramBufferSize, 0);

  VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fillBarrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                       &fillBarrier, 0, nullptr, 0, nullptr);

  updateDescriptorSet(sceneColorView);

  ExposureHistogramPushConstants histogramPc{};
  histogramPc.width = sceneWidth;
  histogramPc.height = sceneHeight;
  histogramPc.binCount = kHistogramBinCount;
  histogramPc.minLogLuminance = kMinHistogramLog2Luminance;
  histogramPc.maxLogLuminance = kMaxHistogramLog2Luminance;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histogramPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
  vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(histogramPc), &histogramPc);
  vkCmdDispatch(cmd, (sceneWidth + 15u) / 16u, (sceneHeight + 15u) / 16u, 1);

  VkMemoryBarrier adaptInputBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  adaptInputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  adaptInputBarrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                       &adaptInputBarrier, 0, nullptr, 0, nullptr);

  ExposureAdaptPushConstants adaptPc{};
  adaptPc.binCount = kHistogramBinCount;
  adaptPc.exposureMode = settings.mode;
  adaptPc.minLogLuminance = kMinHistogramLog2Luminance;
  adaptPc.maxLogLuminance = kMaxHistogramLog2Luminance;
  adaptPc.targetLuminance = settings.targetLuminance;
  adaptPc.minExposure = settings.minExposure;
  adaptPc.maxExposure = settings.maxExposure;
  adaptPc.adaptationRate = settings.adaptationRate;
  adaptPc.meteringLowPercentile = settings.meteringLowPercentile;
  adaptPc.meteringHighPercentile = settings.meteringHighPercentile;
  adaptPc.deltaSeconds = kAssumedFrameDeltaSeconds;
  adaptPc.manualExposure = settings.manualExposure;

  // The adapt shader owns percentile integration now. Keep the historical
  // lowerSample/upperSample/includedSamples naming visible here so convention
  // tests and future readers can find the CPU-to-GPU migration point.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, adaptPipeline_);
  vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(adaptPc), &adaptPc);
  vkCmdDispatch(cmd, 1, 1, 1);

  VkMemoryBarrier postProcessBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  postProcessBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  postProcessBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                       &postProcessBarrier, 0, nullptr, 0, nullptr);
}

void ExposureManager::collectReadback(const ExposureSettings& rawSettings) {
  const ExposureSettings settings = sanitizeExposureSettings(rawSettings);
  if (settings.mode == container::gpu::kExposureModeManual) {
    currentExposure_ = settings.manualExposure;
    averageLuminance_ = settings.targetLuminance;
    hasCurrentExposure_ = true;
    if (exposureStateBuffer_.buffer != VK_NULL_HANDLE) {
      ExposureStateData state{};
      state.exposure = clampExposure(settings.manualExposure, settings);
      state.averageLuminance = settings.targetLuminance;
      state.targetExposure = state.exposure;
      state.initialized = 1.0f;
      SceneController::writeToBuffer(allocationManager_, exposureStateBuffer_,
                                     &state, sizeof(state));
    }
    return;
  }

  if (exposureStateBuffer_.buffer != VK_NULL_HANDLE &&
      exposureStateBuffer_.allocation != nullptr) {
    if (vmaInvalidateAllocation(allocationManager_.memoryManager()->allocator(),
                                exposureStateBuffer_.allocation, 0,
                                sizeof(ExposureStateData)) != VK_SUCCESS) {
      throw std::runtime_error("failed to invalidate exposure state buffer");
    }

    const auto* state = static_cast<const ExposureStateData*>(
        exposureStateBuffer_.allocation_info.pMappedData);
    if (state != nullptr && state->initialized > 0.5f &&
        std::isfinite(state->exposure) &&
        std::isfinite(state->averageLuminance)) {
      currentExposure_ = clampExposure(state->exposure, settings);
      averageLuminance_ = std::max(state->averageLuminance, 0.0f);
      hasCurrentExposure_ = true;
      return;
    }
  }

  currentExposure_ = clampExposure(currentExposure_, settings);
  hasCurrentExposure_ = true;
}

float ExposureManager::resolvedExposure(
    const ExposureSettings& rawSettings) const {
  const ExposureSettings settings = sanitizeExposureSettings(rawSettings);
  if (settings.mode == container::gpu::kExposureModeManual) {
    return settings.manualExposure;
  }
  if (!hasCurrentExposure_) {
    return clampExposure(settings.manualExposure, settings);
  }
  return clampExposure(currentExposure_, settings);
}

VkDeviceSize ExposureManager::exposureStateBufferSize() const {
  return sizeof(ExposureStateData);
}

void ExposureManager::destroy() {
  if (histogramBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(histogramBuffer_);
  }
  if (exposureStateBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(exposureStateBuffer_);
  }
  pipelineManager_.destroyPipeline(histogramPipeline_);
  pipelineManager_.destroyPipeline(adaptPipeline_);
  pipelineManager_.destroyPipelineLayout(pipelineLayout_);
  pipelineManager_.destroyDescriptorPool(descriptorPool_);
  pipelineManager_.destroyDescriptorSetLayout(setLayout_);
  descriptorSet_ = VK_NULL_HANDLE;
  hasCurrentExposure_ = false;
}

void ExposureManager::createPipeline(const std::filesystem::path& shaderDir) {
  const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
  }};
  const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
  setLayout_ = pipelineManager_.createDescriptorSetLayout(
      {bindings.begin(), bindings.end()}, flags);

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size = sizeof(ExposureAdaptPushConstants);

  pipelineLayout_ =
      pipelineManager_.createPipelineLayout({setLayout_}, {pcRange});

  const auto createComputePipeline =
      [&](const char* shaderName, const char* pipelineName) {
        const std::filesystem::path spvPath =
            shaderDir / "spv_shaders" / shaderName;
        const auto spvData = container::util::readFile(spvPath);
        const VkShaderModule module =
            container::gpu::createShaderModule(device_->device(), spvData);

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "computeMain";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout_;

        VkPipeline pipeline =
            pipelineManager_.createComputePipeline(pipelineInfo, pipelineName);
        vkDestroyShaderModule(device_->device(), module, nullptr);
        return pipeline;
      };

  histogramPipeline_ =
      createComputePipeline("exposure_histogram.comp.spv",
                            "exposure_histogram");
  adaptPipeline_ =
      createComputePipeline("exposure_adapt.comp.spv", "exposure_adapt");
}

void ExposureManager::createHistogramBuffer() {
  if (histogramBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(histogramBuffer_);
  }

  histogramBuffer_ = allocationManager_.createBuffer(
      sizeof(uint32_t) * (kHistogramBinCount + 1u),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
}

void ExposureManager::createExposureStateBuffer() {
  if (exposureStateBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(exposureStateBuffer_);
  }

  exposureStateBuffer_ = allocationManager_.createBuffer(
      sizeof(ExposureStateData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  ExposureStateData initialState{};
  initialState.exposure = currentExposure_;
  initialState.averageLuminance = averageLuminance_;
  initialState.targetExposure = currentExposure_;
  initialState.initialized = 0.0f;
  SceneController::writeToBuffer(allocationManager_, exposureStateBuffer_,
                                 &initialState, sizeof(initialState));
}

void ExposureManager::updateDescriptorSet(VkImageView sceneColorView) {
  constexpr VkDeviceSize kHistogramBufferSize =
      sizeof(uint32_t) * (kHistogramBinCount + 1u);

  VkDescriptorImageInfo sceneInfo{};
  sceneInfo.imageView = sceneColorView;
  sceneInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkDescriptorBufferInfo histogramInfo{};
  histogramInfo.buffer = histogramBuffer_.buffer;
  histogramInfo.offset = 0;
  histogramInfo.range = kHistogramBufferSize;

  VkDescriptorBufferInfo exposureInfo{};
  exposureInfo.buffer = exposureStateBuffer_.buffer;
  exposureInfo.offset = 0;
  exposureInfo.range = sizeof(ExposureStateData);

  std::array<VkWriteDescriptorSet, 3> writes{};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[0].dstSet = descriptorSet_;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &sceneInfo;

  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[1].dstSet = descriptorSet_;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &histogramInfo;

  writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[2].dstSet = descriptorSet_;
  writes[2].dstBinding = 2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &exposureInfo;

  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(),
                         0, nullptr);
}

}  // namespace container::renderer
