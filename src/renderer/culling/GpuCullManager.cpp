#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace container::renderer {

using container::gpu::CullPushConstants;
using container::gpu::GpuDrawIndexedIndirectCommand;
using container::gpu::HiZPushConstants;

GpuCullManager::GpuCullManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&            pipelineManager)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , pipelineManager_(pipelineManager) {
}

GpuCullManager::~GpuCullManager() {
  if (inputDrawBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(inputDrawBuffer_);
  if (indirectDrawBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(indirectDrawBuffer_);
  if (drawCountBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(drawCountBuffer_);
  if (occlusionIndirectBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(occlusionIndirectBuffer_);
  if (occlusionCountBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(occlusionCountBuffer_);
  if (statsReadbackBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(statsReadbackBuffer_);
  if (frozenCameraBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(frozenCameraBuffer_);

  pipelineManager_.destroyPipeline(frustumCullPipeline_);
  pipelineManager_.destroyPipelineLayout(frustumCullPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(frustumCullPool_);
  pipelineManager_.destroyDescriptorSetLayout(frustumCullSetLayout_);

  destroyHiZImage();
  if (hizSampler_ != VK_NULL_HANDLE)
    vkDestroySampler(device_->device(), hizSampler_, nullptr);
  pipelineManager_.destroyPipeline(hizPipeline_);
  pipelineManager_.destroyPipelineLayout(hizPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(hizPool_);
  pipelineManager_.destroyDescriptorSetLayout(hizSetLayout_);

  pipelineManager_.destroyPipeline(occlusionCullPipeline_);
  pipelineManager_.destroyPipelineLayout(occlusionCullPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(occlusionCullPool_);
  pipelineManager_.destroyDescriptorSetLayout(occlusionCullSetLayout_);
}

bool GpuCullManager::isReady() const {
  return frustumCullPipeline_ != VK_NULL_HANDLE &&
         device_->enabledFeatures().drawIndirectFirstInstance == VK_TRUE;
}

bool GpuCullManager::canRecordOcclusionCull() const {
  return isReady() &&
         frustumDrawsValid_ &&
         hizGeneratedThisFrame_ &&
         occlusionCullPipeline_ != VK_NULL_HANDLE &&
         occlusionCullSet_ != VK_NULL_HANDLE &&
         occlusionIndirectBuffer_.buffer != VK_NULL_HANDLE &&
         occlusionCountBuffer_.buffer != VK_NULL_HANDLE &&
         drawCountBuffer_.buffer != VK_NULL_HANDLE &&
         indirectDrawBuffer_.buffer != VK_NULL_HANDLE &&
         hizFullView_ != VK_NULL_HANDLE &&
         hizSampler_ != VK_NULL_HANDLE &&
         !hizMipViews_.empty() &&
         hizSets_.size() == hizMipLevels_;
}

void GpuCullManager::beginFrameCulling() {
  frustumDrawsValid_ = false;
  hizGeneratedThisFrame_ = false;
  occlusionDrawsValid_ = false;
}

// ---------------------------------------------------------------------------
// Resource creation
// ---------------------------------------------------------------------------

void GpuCullManager::createResources(const std::filesystem::path& shaderDir) {
  createFrustumCullPipeline(shaderDir);
  createHiZPipeline(shaderDir);
  createOcclusionCullPipeline(shaderDir);
}

bool GpuCullManager::ensureBufferCapacity(uint32_t maxObjectCount) {
  if (maxObjectCount <= maxObjectCount_ &&
      indirectDrawBuffer_.buffer != VK_NULL_HANDLE) {
    return false;
  }

  const uint32_t capacity = std::max(maxObjectCount, 64u);

  if (inputDrawBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(inputDrawBuffer_);
  if (indirectDrawBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(indirectDrawBuffer_);
  if (drawCountBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(drawCountBuffer_);
  if (occlusionIndirectBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(occlusionIndirectBuffer_);
  if (occlusionCountBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(occlusionCountBuffer_);

  // Input draw command SSBO (CPU-writable).
  inputDrawBuffer_ = allocationManager_.createBuffer(
      sizeof(GpuDrawIndexedIndirectCommand) * capacity,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  // Output indirect draw buffer (GPU-only, also usable as indirect source).
  indirectDrawBuffer_ = allocationManager_.createBuffer(
      sizeof(GpuDrawIndexedIndirectCommand) * capacity,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  // Draw count buffer (single uint32, GPU-only + indirect).
  drawCountBuffer_ = allocationManager_.createBuffer(
      sizeof(uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  // Occlusion-culled output (second pass for G-Buffer).
  occlusionIndirectBuffer_ = allocationManager_.createBuffer(
      sizeof(GpuDrawIndexedIndirectCommand) * capacity,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  occlusionCountBuffer_ = allocationManager_.createBuffer(
      sizeof(uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  // Stats readback buffer: 2 × uint32_t (frustum count + occlusion count).
  if (statsReadbackBuffer_.buffer != VK_NULL_HANDLE)
    allocationManager_.destroyBuffer(statsReadbackBuffer_);
  statsReadbackBuffer_ = allocationManager_.createBuffer(
      sizeof(uint32_t) * 2,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  maxObjectCount_ = capacity;

  writeDescriptorSets();
  return true;
}

// ---------------------------------------------------------------------------
// Upload draw commands
// ---------------------------------------------------------------------------

void GpuCullManager::uploadDrawCommands(
    const std::vector<DrawCommand>& commands) {
  if (inputDrawBuffer_.buffer == VK_NULL_HANDLE || commands.empty()) return;

  const uint32_t count =
      std::min(static_cast<uint32_t>(commands.size()), maxObjectCount_);

  // Convert DrawCommand to GpuDrawIndexedIndirectCommand.
  // objectIndex is encoded in firstInstance so vertex shaders can read it.
  uploadScratch_.resize(count);
  auto& gpuCmds = uploadScratch_;
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

  lastStats_.totalInputCount = count;
}

// ---------------------------------------------------------------------------
// Frustum cull dispatch
// ---------------------------------------------------------------------------

void GpuCullManager::dispatchFrustumCull(VkCommandBuffer cmd,
                                          VkBuffer cameraBuffer,
                                          VkDeviceSize cameraBufferSize,
                                          uint32_t objectCount) {
  frustumDrawsValid_ = false;
  if (frustumCullPipeline_ == VK_NULL_HANDLE ||
      indirectDrawBuffer_.buffer == VK_NULL_HANDLE ||
      drawCountBuffer_.buffer == VK_NULL_HANDLE ||
      frustumCullSet_ == VK_NULL_HANDLE ||
      objectCount == 0) return;

  // Zero the draw count buffer.
  vkCmdFillBuffer(cmd, drawCountBuffer_.buffer, 0, sizeof(uint32_t), 0);

  VkMemoryBarrier fillBarrier{};
  fillBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

  // Update camera buffer descriptor for this frame.
  // When culling is frozen, use the snapshot buffer instead of the live one.
  {
    const VkBuffer activeCam = (cullingFrozen_ && frozenCameraBuffer_.buffer != VK_NULL_HANDLE)
                                 ? frozenCameraBuffer_.buffer : cameraBuffer;
    VkDescriptorBufferInfo camInfo{activeCam, 0, cameraBufferSize};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = frustumCullSet_;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo     = &camInfo;
    vkUpdateDescriptorSets(device_->device(), 1, &w, 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, frustumCullPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          frustumCullPipelineLayout_, 0, 1,
                          &frustumCullSet_, 0, nullptr);

  CullPushConstants pc{};
  pc.objectCount = objectCount;
  vkCmdPushConstants(cmd, frustumCullPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(CullPushConstants), &pc);

  const uint32_t groupCount = (objectCount + 63) / 64;
  vkCmdDispatch(cmd, groupCount, 1, 1);

  // Barrier: compute writes → indirect draw reads + occlusion cull reads.
  VkMemoryBarrier barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  frustumDrawsValid_ = true;
}

// ---------------------------------------------------------------------------
// Hi-Z generation
// ---------------------------------------------------------------------------

void GpuCullManager::ensureHiZImage(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) return;

  if (width == hizWidth_ && height == hizHeight_ && hizImage_ != VK_NULL_HANDLE)
    return;

  destroyHiZImage();

  hizWidth_  = width;
  hizHeight_ = height;
  hizMipLevels_ = static_cast<uint32_t>(
      std::floor(std::log2(static_cast<float>(std::max(width, height))))) + 1;
  hizInitialized_ = false;

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType     = VK_IMAGE_TYPE_2D;
  ci.format        = VK_FORMAT_R32_SFLOAT;
  ci.extent        = {width, height, 1};
  ci.mipLevels     = hizMipLevels_;
  ci.arrayLayers   = 1;
  ci.samples       = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
  ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  if (vmaCreateImage(allocationManager_.memoryManager()->allocator(), &ci, &ai,
                     &hizImage_, &hizAllocation_, nullptr) != VK_SUCCESS)
    throw std::runtime_error("failed to create Hi-Z image");

  // Full-mip view for sampling in occlusion cull.
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image    = hizImage_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format   = VK_FORMAT_R32_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels_, 0, 1};
  if (vkCreateImageView(device_->device(), &vi, nullptr, &hizFullView_) != VK_SUCCESS)
    throw std::runtime_error("failed to create Hi-Z full image view");

  // Per-mip views for storage writes.
  hizMipViews_.resize(hizMipLevels_);
  for (uint32_t m = 0; m < hizMipLevels_; ++m) {
    VkImageViewCreateInfo mvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    mvi.image    = hizImage_;
    mvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    mvi.format   = VK_FORMAT_R32_SFLOAT;
    mvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
    if (vkCreateImageView(device_->device(), &mvi, nullptr, &hizMipViews_[m]) != VK_SUCCESS)
      throw std::runtime_error("failed to create Hi-Z mip view " + std::to_string(m));
  }

  // Sampler: nearest, clamp-to-edge. Recreate it with the image because maxLod
  // depends on the mip count, which can change after swapchain resize.
  if (hizSampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(device_->device(), hizSampler_, nullptr);
    hizSampler_ = VK_NULL_HANDLE;
  }
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter    = VK_FILTER_NEAREST;
  si.minFilter    = VK_FILTER_NEAREST;
  si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.minLod       = 0.0f;
  si.maxLod       = static_cast<float>(hizMipLevels_ - 1u);
  if (vkCreateSampler(device_->device(), &si, nullptr, &hizSampler_) != VK_SUCCESS)
    throw std::runtime_error("failed to create Hi-Z sampler");

  createHiZDescriptorSets();
}

void GpuCullManager::destroyHiZImage() {
  VkDevice dev = device_->device();
  pipelineManager_.destroyDescriptorPool(hizPool_);
  hizSets_.clear();

  for (auto v : hizMipViews_)
    if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
  hizMipViews_.clear();
  if (hizFullView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(dev, hizFullView_, nullptr);
    hizFullView_ = VK_NULL_HANDLE;
  }
  if (hizImage_ != VK_NULL_HANDLE) {
    vmaDestroyImage(allocationManager_.memoryManager()->allocator(), hizImage_, hizAllocation_);
    hizImage_      = VK_NULL_HANDLE;
    hizAllocation_ = nullptr;
  }
  hizWidth_  = 0;
  hizHeight_ = 0;
  hizMipLevels_ = 0;
  hizInitialized_ = false;
}

void GpuCullManager::dispatchHiZGenerate(VkCommandBuffer cmd,
                                          VkImageView depthView,
                                          VkSampler depthSampler,
                                          uint32_t width, uint32_t height) {
  hizGeneratedThisFrame_ = false;
  if (width == 0 || height == 0) return;

  if (hizPipeline_ == VK_NULL_HANDLE || hizImage_ == VK_NULL_HANDLE ||
      hizMipViews_.empty() || hizSets_.size() != hizMipLevels_ ||
      depthView == VK_NULL_HANDLE || depthSampler == VK_NULL_HANDLE) return;

  // Transition Hi-Z image to GENERAL for storage writes.
  {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = hizInitialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout           = hizInitialized_
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.image               = hizImage_;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels_, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         hizInitialized_ ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipeline_);

  uint32_t srcW = width;
  uint32_t srcH = height;

  for (uint32_t mip = 0; mip < hizMipLevels_; ++mip) {
    // Update descriptor set for this mip level.
    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageLayout = (mip == 0)
        ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_GENERAL;
    srcInfo.imageView = (mip == 0) ? depthView : hizMipViews_[mip - 1];

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = (mip == 0) ? depthSampler : hizSampler_;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstInfo.imageView   = hizMipViews_[mip];

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet          = hizSets_[mip];
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo      = &srcInfo;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet          = hizSets_[mip];
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo      = &samplerInfo;
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet          = hizSets_[mip];
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo      = &dstInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            hizPipelineLayout_, 0, 1, &hizSets_[mip], 0,
                            nullptr);

    HiZPushConstants hpc{};
    hpc.srcWidth  = srcW;
    hpc.srcHeight = srcH;
    hpc.dstMipLevel = mip;
    vkCmdPushConstants(cmd, hizPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(HiZPushConstants), &hpc);

    const uint32_t dstW = (mip == 0) ? srcW : std::max(srcW >> 1, 1u);
    const uint32_t dstH = (mip == 0) ? srcH : std::max(srcH >> 1, 1u);
    vkCmdDispatch(cmd, (dstW + 7) / 8, (dstH + 7) / 8, 1);

    // Barrier between mip levels.
    if (mip + 1 < hizMipLevels_) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
      b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
      b.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
      b.image            = hizImage_;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
    }

    srcW = dstW;
    srcH = dstH;
  }

  // Final barrier: Hi-Z image is ready for sampling in occlusion cull.
  {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image            = hizImage_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMipLevels_, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    hizInitialized_ = true;
    hizGeneratedThisFrame_ = true;
  }
}

// ---------------------------------------------------------------------------
// Occlusion cull
// ---------------------------------------------------------------------------

void GpuCullManager::dispatchOcclusionCull(VkCommandBuffer cmd,
                                            VkBuffer cameraBuffer,
                                            VkDeviceSize cameraBufferSize,
                                            uint32_t objectCount) {
  occlusionDrawsValid_ = false;
  if (!canRecordOcclusionCull() || objectCount == 0) return;

  // Zero the occlusion draw count.
  vkCmdFillBuffer(cmd, occlusionCountBuffer_.buffer, 0, sizeof(uint32_t), 0);

  VkMemoryBarrier fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &fillBarrier, 0, nullptr, 0, nullptr);

  // Update occlusion cull descriptor set.
  // When culling is frozen, use the snapshot buffer instead of the live one.
  {
    const VkBuffer activeCam = (cullingFrozen_ && frozenCameraBuffer_.buffer != VK_NULL_HANDLE)
                                 ? frozenCameraBuffer_.buffer : cameraBuffer;
    VkDescriptorBufferInfo camInfo{activeCam, 0, cameraBufferSize};

    // Binding 2: input draws from frustum-culled indirect buffer.
    VkDescriptorBufferInfo inputInfo{
        indirectDrawBuffer_.buffer, 0,
        sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
    // Binding 3: output draws (occlusion-culled).
    VkDescriptorBufferInfo outputInfo{
        occlusionIndirectBuffer_.buffer, 0,
        sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
    // Binding 4: occlusion draw count (RW).
    VkDescriptorBufferInfo countInfo{
        occlusionCountBuffer_.buffer, 0, sizeof(uint32_t)};
    // Binding 5: Hi-Z pyramid (sampled image).
    VkDescriptorImageInfo hizInfo{};
    hizInfo.imageView   = hizFullView_;
    hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Binding 6: frustum-culled draw count (read-only, from frustum cull pass).
    VkDescriptorBufferInfo frustumCountInfo{
        drawCountBuffer_.buffer, 0, sizeof(uint32_t)};
    // Binding 7: Hi-Z sampler.
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = hizSampler_;

    std::array<VkWriteDescriptorSet, 7> writes{};
    // Binding 0: camera UBO.
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = occlusionCullSet_; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &camInfo;
    // Binding 1: object SSBO — written via updateObjectSsboDescriptor; skip here.
    // Binding 2: input draws (frustum-culled output).
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = occlusionCullSet_; writes[1].dstBinding = 2;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &inputInfo;
    // Binding 3: output draws (occlusion-culled).
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = occlusionCullSet_; writes[2].dstBinding = 3;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo     = &outputInfo;
    // Binding 4: occlusion draw count.
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[3].dstSet = occlusionCullSet_; writes[3].dstBinding = 4;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo     = &countInfo;
    // Binding 5: Hi-Z sampled image.
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[4].dstSet = occlusionCullSet_; writes[4].dstBinding = 5;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[4].pImageInfo      = &hizInfo;
    // Binding 6: frustum-culled draw count (read-only).
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[5].dstSet = occlusionCullSet_; writes[5].dstBinding = 6;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo     = &frustumCountInfo;
    // Binding 7: Hi-Z sampler.
    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[6].dstSet = occlusionCullSet_; writes[6].dstBinding = 7;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[6].pImageInfo      = &samplerInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, occlusionCullPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          occlusionCullPipelineLayout_, 0, 1,
                          &occlusionCullSet_, 0, nullptr);

  CullPushConstants pc{};
  pc.objectCount = objectCount;
  vkCmdPushConstants(cmd, occlusionCullPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(CullPushConstants), &pc);

  const uint32_t groupCount = (objectCount + 63) / 64;
  vkCmdDispatch(cmd, groupCount, 1, 1);

  // Barrier: compute writes → indirect draw reads.
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  occlusionDrawsValid_ = true;
}

// ---------------------------------------------------------------------------
// Indirect draw
// ---------------------------------------------------------------------------

void GpuCullManager::drawIndirect(VkCommandBuffer cmd) const {
  if (indirectDrawBuffer_.buffer == VK_NULL_HANDLE ||
      drawCountBuffer_.buffer == VK_NULL_HANDLE ||
      !frustumDrawsValid_) return;

  vkCmdDrawIndexedIndirectCount(
      cmd,
      indirectDrawBuffer_.buffer, 0,
      drawCountBuffer_.buffer, 0,
      maxObjectCount_,
      sizeof(GpuDrawIndexedIndirectCommand));
}

void GpuCullManager::drawIndirectOccluded(VkCommandBuffer cmd) const {
  if (occlusionIndirectBuffer_.buffer == VK_NULL_HANDLE ||
      occlusionCountBuffer_.buffer == VK_NULL_HANDLE ||
      !occlusionDrawsValid_) return;

  vkCmdDrawIndexedIndirectCount(
      cmd,
      occlusionIndirectBuffer_.buffer, 0,
      occlusionCountBuffer_.buffer, 0,
      maxObjectCount_,
      sizeof(GpuDrawIndexedIndirectCommand));
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void GpuCullManager::createFrustumCullPipeline(
    const std::filesystem::path& shaderDir) {
  // Descriptor set layout: binding 0 = camera UBO, 1 = object SSBO,
  // 2 = input draw SSBO, 3 = output draw SSBO, 4 = draw count SSBO.
  {
    const std::array<VkDescriptorSetLayoutBinding, 5> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    frustumCullSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  frustumCullPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}},
      1, 0);

  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = frustumCullPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &frustumCullSetLayout_;
    if (vkAllocateDescriptorSets(device_->device(), &ai,
                                 &frustumCullSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate frustum cull descriptor set");
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size       = sizeof(CullPushConstants);

  frustumCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {frustumCullSetLayout_}, {pcRange});

  auto compPath = shaderDir / "spv_shaders" / "frustum_cull.comp.spv";
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
  ci.layout = frustumCullPipelineLayout_;

  frustumCullPipeline_ =
      pipelineManager_.createComputePipeline(ci, "frustum_cull");

  vkDestroyShaderModule(device_->device(), compModule, nullptr);
}

void GpuCullManager::createHiZPipeline(
    const std::filesystem::path& shaderDir) {
  auto compPath = shaderDir / "spv_shaders" / "hiz_generate.comp.spv";
  if (!std::filesystem::exists(compPath)) return;

  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    hizSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size       = sizeof(HiZPushConstants);

  hizPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {hizSetLayout_}, {pcRange});

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
  ci.layout = hizPipelineLayout_;

  hizPipeline_ = pipelineManager_.createComputePipeline(ci, "hiz_generate");
  vkDestroyShaderModule(device_->device(), compModule, nullptr);
}

void GpuCullManager::createHiZDescriptorSets() {
  if (hizSetLayout_ == VK_NULL_HANDLE || hizMipLevels_ == 0) return;

  pipelineManager_.destroyDescriptorPool(hizPool_);
  hizSets_.clear();

  hizPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, hizMipLevels_},
       {VK_DESCRIPTOR_TYPE_SAMPLER, hizMipLevels_},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hizMipLevels_}},
      hizMipLevels_, 0);

  std::vector<VkDescriptorSetLayout> layouts(hizMipLevels_, hizSetLayout_);
  hizSets_.resize(hizMipLevels_, VK_NULL_HANDLE);

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = hizPool_;
  ai.descriptorSetCount = hizMipLevels_;
  ai.pSetLayouts        = layouts.data();
  if (vkAllocateDescriptorSets(device_->device(), &ai, hizSets_.data()) !=
      VK_SUCCESS) {
    hizSets_.clear();
    throw std::runtime_error("failed to allocate Hi-Z descriptor sets");
  }
}

void GpuCullManager::createOcclusionCullPipeline(
    const std::filesystem::path& shaderDir) {
  auto compPath = shaderDir / "spv_shaders" / "occlusion_cull.comp.spv";
  if (!std::filesystem::exists(compPath)) return;

  {
    const std::array<VkDescriptorSetLayoutBinding, 8> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    occlusionCullSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  occlusionCullPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
       {VK_DESCRIPTOR_TYPE_SAMPLER, 1}},
      1, 0);

  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = occlusionCullPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &occlusionCullSetLayout_;
    if (vkAllocateDescriptorSets(device_->device(), &ai,
                                 &occlusionCullSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate occlusion cull descriptor set");
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.size       = sizeof(CullPushConstants);

  occlusionCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {occlusionCullSetLayout_}, {pcRange});

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
  ci.layout = occlusionCullPipelineLayout_;

  occlusionCullPipeline_ =
      pipelineManager_.createComputePipeline(ci, "occlusion_cull");
  vkDestroyShaderModule(device_->device(), compModule, nullptr);
}

// ---------------------------------------------------------------------------
// Descriptor writes
// ---------------------------------------------------------------------------

void GpuCullManager::writeDescriptorSets() {
  if (frustumCullSet_ == VK_NULL_HANDLE) return;

  // Bindings 1-4: object SSBO, input draw, output draw, draw count.
  // Binding 0 (camera) is updated per-frame in dispatchFrustumCull.
  VkDescriptorBufferInfo inputInfo{
      inputDrawBuffer_.buffer, 0,
      sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
  VkDescriptorBufferInfo outputInfo{
      indirectDrawBuffer_.buffer, 0,
      sizeof(GpuDrawIndexedIndirectCommand) * maxObjectCount_};
  VkDescriptorBufferInfo countInfo{
      drawCountBuffer_.buffer, 0, sizeof(uint32_t)};

  std::array<VkWriteDescriptorSet, 3> writes{};
  // Binding 2: input draw commands.
  writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet          = frustumCullSet_;
  writes[0].dstBinding      = 2;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo     = &inputInfo;
  // Binding 3: output indirect commands.
  writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet          = frustumCullSet_;
  writes[1].dstBinding      = 3;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo     = &outputInfo;
  // Binding 4: draw count.
  writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet          = frustumCullSet_;
  writes[2].dstBinding      = 4;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo     = &countInfo;

  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
}

void GpuCullManager::updateObjectSsboDescriptor(
    VkBuffer objectBuffer, VkDeviceSize objectBufferSize) {
  if (objectBuffer == objectSsboBuffer_ &&
      objectBufferSize == objectSsboSize_) {
    return;
  }
  objectSsboBuffer_ = objectBuffer;
  objectSsboSize_   = objectBufferSize;

  VkDescriptorBufferInfo objInfo{objectBuffer, 0, objectBufferSize};

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(2);

  if (frustumCullSet_ != VK_NULL_HANDLE) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = frustumCullSet_;
    w.dstBinding      = 1;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo     = &objInfo;
    writes.push_back(w);
  }

  if (occlusionCullSet_ != VK_NULL_HANDLE) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = occlusionCullSet_;
    w.dstBinding      = 1;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo     = &objInfo;
    writes.push_back(w);
  }

  if (!writes.empty())
    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
// Stats readback
// ---------------------------------------------------------------------------

void GpuCullManager::scheduleStatsReadback(VkCommandBuffer cmd) {
  if (statsReadbackBuffer_.buffer == VK_NULL_HANDLE) return;

  if (!frustumDrawsValid_ || drawCountBuffer_.buffer == VK_NULL_HANDLE) {
    vkCmdFillBuffer(cmd, statsReadbackBuffer_.buffer, 0,
                    sizeof(uint32_t) * 2, 0);

    VkMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    postBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &postBarrier, 0, nullptr, 0, nullptr);
    return;
  }

  if (occlusionCountBuffer_.buffer == VK_NULL_HANDLE) return;

  // Barrier: ensure all compute writes to count buffers are visible to transfer.
  VkMemoryBarrier preBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  preBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  preBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &preBarrier, 0, nullptr, 0, nullptr);

  // Copy frustum-culled draw count → statsReadbackBuffer_[0].
  VkBufferCopy frustumCopy{};
  frustumCopy.srcOffset = 0;
  frustumCopy.dstOffset = 0;
  frustumCopy.size      = sizeof(uint32_t);
  vkCmdCopyBuffer(cmd, drawCountBuffer_.buffer,
                  statsReadbackBuffer_.buffer, 1, &frustumCopy);

  // Copy occlusion-culled draw count → statsReadbackBuffer_[1].
  const VkBuffer occlusionStatsSource =
      occlusionDrawsValid_ ? occlusionCountBuffer_.buffer
                           : drawCountBuffer_.buffer;

  VkBufferCopy occlusionCopy{};
  occlusionCopy.srcOffset = 0;
  occlusionCopy.dstOffset = sizeof(uint32_t);
  occlusionCopy.size      = sizeof(uint32_t);
  vkCmdCopyBuffer(cmd, occlusionStatsSource,
                  statsReadbackBuffer_.buffer, 1, &occlusionCopy);

  // Barrier: transfer writes → host reads (visible after fence).
  VkMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  postBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &postBarrier, 0, nullptr, 0, nullptr);
}

void GpuCullManager::collectStats() {
  if (statsReadbackBuffer_.buffer == VK_NULL_HANDLE ||
      statsReadbackBuffer_.allocation == nullptr) return;

  VmaAllocationInfo allocInfo{};
  vmaGetAllocationInfo(allocationManager_.memoryManager()->allocator(),
                       statsReadbackBuffer_.allocation, &allocInfo);
  if (allocInfo.pMappedData == nullptr) return;

  vmaInvalidateAllocation(allocationManager_.memoryManager()->allocator(),
                          statsReadbackBuffer_.allocation, 0,
                          sizeof(uint32_t) * 2);

  const auto* data = static_cast<const uint32_t*>(allocInfo.pMappedData);
  lastStats_.frustumPassedCount   = data[0];
  lastStats_.occlusionPassedCount = data[1];
}

// ---------------------------------------------------------------------------
// Freeze-culling
// ---------------------------------------------------------------------------

void GpuCullManager::freezeCulling(VkCommandBuffer cmd, VkBuffer liveCameraBuffer,
                                    VkDeviceSize cameraBufferSize) {
  if (liveCameraBuffer == VK_NULL_HANDLE || cameraBufferSize == 0) return;

  // (Re)create the frozen camera buffer if needed.
  if (frozenCameraBuffer_.buffer == VK_NULL_HANDLE) {
    frozenCameraBuffer_ = allocationManager_.createBuffer(
        cameraBufferSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
  }

  // Copy the live camera buffer → frozen buffer.
  VkBufferCopy copy{};
  copy.size = cameraBufferSize;
  vkCmdCopyBuffer(cmd, liveCameraBuffer, frozenCameraBuffer_.buffer, 1, &copy);

  // Barrier: transfer → uniform read (for subsequent cull dispatches).
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 1, &barrier, 0, nullptr, 0, nullptr);

  cullingFrozen_ = true;
}

void GpuCullManager::unfreezeCulling() {
  cullingFrozen_ = false;
}

}  // namespace container::renderer
