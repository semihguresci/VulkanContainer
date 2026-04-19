#include "Container/renderer/ShadowManager.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace container::renderer {

using container::gpu::kShadowCascadeCount;
using container::gpu::kShadowMapResolution;
using container::gpu::ShadowData;

ShadowManager::ShadowManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&              pipelineManager)
    : device_(std::move(device)),
      allocationManager_(allocationManager),
      pipelineManager_(pipelineManager) {}

ShadowManager::~ShadowManager() {
  destroy();
}

// ---------------------------------------------------------------------------
void ShadowManager::createResources(VkFormat depthFormat) {
  depthFormat_ = depthFormat;
  VkDevice dev = device_->device();

  // ---- Shadow atlas image (2D array, kShadowCascadeCount layers) ----------
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = depthFormat_;
    ii.extent      = {kShadowMapResolution, kShadowMapResolution, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = kShadowCascadeCount;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &shadowAtlasImage_,
                       &shadowAtlasAllocation_, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow atlas image");
  }

  // ---- Full array view (for shader sampling) ------------------------------
  {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = shadowAtlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = depthFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascadeCount};
    if (vkCreateImageView(dev, &vi, nullptr, &shadowAtlasArrayView_) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow atlas array view");
  }

  // ---- Per-cascade single-layer views (for framebuffer attachment) ---------
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = shadowAtlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = depthFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &cascadeViews_[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow cascade view");
  }

  // ---- Comparison sampler for PCF -----------------------------------------
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE;
    si.compareOp     = VK_COMPARE_OP_GREATER;
    si.minLod       = 0.0f;
    si.maxLod       = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &shadowSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow sampler");
  }

  // ---- Shadow UBO ---------------------------------------------------------
  shadowUbo_ = allocationManager_.createBuffer(
      sizeof(ShadowData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  // ---- Descriptor set layout (shadow UBO only — binding 0) ----------------
  {
    const VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    descriptorSetLayout_ =
        pipelineManager_.createDescriptorSetLayout({binding}, {0});
  }

  // ---- Descriptor pool + set ----------------------------------------------
  descriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}, 1, 0);

  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(dev, &ai, &descriptorSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate shadow descriptor set");
  }

  // Write UBO to the descriptor set.
  VkDescriptorBufferInfo bufInfo{shadowUbo_.buffer, 0, sizeof(ShadowData)};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet          = descriptorSet_;
  write.dstBinding      = 0;
  write.descriptorCount = 1;
  write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.pBufferInfo     = &bufInfo;
  vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

// ---------------------------------------------------------------------------
void ShadowManager::createFramebuffers(VkRenderPass shadowRenderPass) {
  destroyFramebuffers();
  VkDevice dev = device_->device();
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass      = shadowRenderPass;
    fbi.attachmentCount = 1;
    fbi.pAttachments    = &cascadeViews_[i];
    fbi.width           = kShadowMapResolution;
    fbi.height          = kShadowMapResolution;
    fbi.layers          = 1;
    if (vkCreateFramebuffer(dev, &fbi, nullptr, &framebuffers_[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow framebuffer");
  }
}

// ---------------------------------------------------------------------------
void ShadowManager::destroyFramebuffers() {
  VkDevice dev = device_->device();
  for (auto& fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(dev, fb, nullptr);
      fb = VK_NULL_HANDLE;
    }
  }
}

// ---------------------------------------------------------------------------
void ShadowManager::destroy() {
  VkDevice dev = device_->device();
  if (!dev) return;

  destroyFramebuffers();

  if (shadowSampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(dev, shadowSampler_, nullptr);
    shadowSampler_ = VK_NULL_HANDLE;
  }

  for (auto& v : cascadeViews_) {
    if (v != VK_NULL_HANDLE) {
      vkDestroyImageView(dev, v, nullptr);
      v = VK_NULL_HANDLE;
    }
  }
  if (shadowAtlasArrayView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(dev, shadowAtlasArrayView_, nullptr);
    shadowAtlasArrayView_ = VK_NULL_HANDLE;
  }
  if (shadowAtlasImage_ != VK_NULL_HANDLE && shadowAtlasAllocation_ != nullptr) {
    vmaDestroyImage(allocationManager_.memoryManager()->allocator(),
                    shadowAtlasImage_, shadowAtlasAllocation_);
    shadowAtlasImage_      = VK_NULL_HANDLE;
    shadowAtlasAllocation_ = nullptr;
  }
  if (shadowUbo_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(shadowUbo_);
  }

  descriptorSet_       = VK_NULL_HANDLE;
  descriptorPool_      = VK_NULL_HANDLE;
  descriptorSetLayout_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
void ShadowManager::computeCascadeSplits(float nearPlane, float farPlane) {
  const float ratio = farPlane / nearPlane;
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    const float p = static_cast<float>(i + 1) /
                    static_cast<float>(kShadowCascadeCount);
    const float logSplit  = nearPlane * std::pow(ratio, p);
    const float linSplit  = nearPlane + (farPlane - nearPlane) * p;
    cascadeSplits_[i] = kCascadeLambda * logSplit +
                        (1.0f - kCascadeLambda) * linSplit;
  }
}

// ---------------------------------------------------------------------------
glm::mat4 ShadowManager::computeCascadeViewProj(
    uint32_t cascadeIndex,
    const glm::vec3& lightDirection,
    const glm::mat4& cameraView,
    const glm::mat4& cameraProj,
    float cameraNear,
    float cameraFar) const {
  const float cascadeNear =
      (cascadeIndex == 0) ? cameraNear : cascadeSplits_[cascadeIndex - 1];
  const float cascadeFar = cascadeSplits_[cascadeIndex];

  constexpr std::array<glm::vec2, 4> ndcXY = {{
      {-1.0f, -1.0f},
      { 1.0f, -1.0f},
      { 1.0f,  1.0f},
      {-1.0f,  1.0f},
  }};

  const glm::mat4 invVP = glm::inverse(cameraProj * cameraView);

  std::array<glm::vec3, 4> nearCorners{};
  std::array<glm::vec3, 4> farCorners{};
  for (size_t i = 0; i < ndcXY.size(); ++i) {
    glm::vec4 nearCorner = invVP * glm::vec4(ndcXY[i], 1.0f, 1.0f);
    glm::vec4 farCorner  = invVP * glm::vec4(ndcXY[i], 0.0f, 1.0f);
    nearCorners[i] = glm::vec3(nearCorner) / nearCorner.w;
    farCorners[i]  = glm::vec3(farCorner) / farCorner.w;
  }

  const float cameraDepthRange = std::max(cameraFar - cameraNear, 1e-4f);
  const float tNear =
      std::clamp((cascadeNear - cameraNear) / cameraDepthRange, 0.0f, 1.0f);
  const float tFar =
      std::clamp((cascadeFar - cameraNear) / cameraDepthRange, 0.0f, 1.0f);

  std::array<glm::vec3, 8> sliceCorners{};
  for (size_t i = 0; i < nearCorners.size(); ++i) {
    const glm::vec3 ray = farCorners[i] - nearCorners[i];
    sliceCorners[i]     = nearCorners[i] + ray * tNear;
    sliceCorners[i + 4] = nearCorners[i] + ray * tFar;
  }

  glm::vec3 center(0.0f);
  for (const auto& corner : sliceCorners) center += corner;
  center /= static_cast<float>(sliceCorners.size());

  float cascadeRadius = 0.0f;
  for (const auto& corner : sliceCorners)
    cascadeRadius = std::max(cascadeRadius, glm::length(corner - center));
  cascadeRadius = std::max(cascadeRadius, 1.0f);

  glm::vec3 lightDir = lightDirection;
  if (glm::dot(lightDir, lightDir) < 1e-6f) lightDir = glm::vec3(-0.45f, -1.0f, -0.3f);
  lightDir = glm::normalize(lightDir);

  const glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.99f)
      ? glm::vec3(0, 0, 1)
      : glm::vec3(0, 1, 0);
  const float     lightDistance = cascadeRadius * 2.0f;
  const glm::mat4 lightView = container::math::lookAt(
      center - lightDir * lightDistance, center, up);

  glm::vec3 minBounds( std::numeric_limits<float>::max());
  glm::vec3 maxBounds(-std::numeric_limits<float>::max());
  for (const auto& corner : sliceCorners) {
    const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
    minBounds = glm::min(minBounds, lightSpaceCorner);
    maxBounds = glm::max(maxBounds, lightSpaceCorner);
  }

  const float orthoWidth = std::max(maxBounds.x - minBounds.x, 1e-3f);
  const float orthoHeight = std::max(maxBounds.y - minBounds.y, 1e-3f);
  const float texelSizeX =
      orthoWidth / static_cast<float>(kShadowMapResolution);
  const float texelSizeY =
      orthoHeight / static_cast<float>(kShadowMapResolution);
  glm::vec2 orthoCenter{
      (minBounds.x + maxBounds.x) * 0.5f,
      (minBounds.y + maxBounds.y) * 0.5f,
  };
  orthoCenter.x = std::floor(orthoCenter.x / texelSizeX) * texelSizeX;
  orthoCenter.y = std::floor(orthoCenter.y / texelSizeY) * texelSizeY;
  minBounds.x = orthoCenter.x - orthoWidth * 0.5f;
  maxBounds.x = orthoCenter.x + orthoWidth * 0.5f;
  minBounds.y = orthoCenter.y - orthoHeight * 0.5f;
  maxBounds.y = orthoCenter.y + orthoHeight * 0.5f;

  // Extend away from the light so off-frustum casters can still project into
  // the cascade. The reverse-Z ortho helper expects positive near/far
  // distances, so convert the signed light-view Z bounds after expansion.
  const float zExtend = std::max(maxBounds.z - minBounds.z, 1.0f);
  minBounds.z -= zExtend;

  const float nearPlane = std::max(0.01f, -maxBounds.z);
  const float farPlane  = std::max(nearPlane + 0.01f, -minBounds.z);

  const glm::mat4 lightProj = container::math::orthoRH_ReverseZ(
      minBounds.x, maxBounds.x, minBounds.y, maxBounds.y, nearPlane, farPlane);
  return lightProj * lightView;
}

// ---------------------------------------------------------------------------
void ShadowManager::update(const container::scene::BaseCamera* camera,
                           float aspectRatio,
                           const glm::vec3& lightDirection) {
  if (!camera) return;

  // Get near/far from camera (cast to PerspectiveCamera for near/far access).
  const auto* perspCam =
      dynamic_cast<const container::scene::PerspectiveCamera*>(camera);
  const float nearPlane = perspCam ? perspCam->nearPlane() : 0.1f;
  const float farPlane  = perspCam ? perspCam->farPlane()  : 100.0f;

  computeCascadeSplits(nearPlane, farPlane);

  const glm::mat4 cameraView = camera->viewMatrix();
  glm::mat4 cameraProj = camera->projectionMatrix(aspectRatio);

  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    shadowData_.cascades[i].viewProj =
        computeCascadeViewProj(i, lightDirection, cameraView, cameraProj,
                               nearPlane, farPlane);
    shadowData_.cascades[i].splitDepth = cascadeSplits_[i];
  }

  // Upload to UBO.
  if (shadowUbo_.buffer != VK_NULL_HANDLE) {
    void* mapped = shadowUbo_.allocation_info.pMappedData;
    if (mapped) {
      std::memcpy(mapped, &shadowData_, sizeof(ShadowData));
    }
  }
}

}  // namespace container::renderer
