#include "Container/renderer/FrameResourceManager.h"

#include "Container/common/CommonVMA.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::renderer {

// -----------------------------------------------------------------------
FrameResourceManager::FrameResourceManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&            pipelineManager,
    container::gpu::SwapChainManager&                     swapChainManager,
    VkCommandPool                                  commandPool)
    : device_(std::move(device)),
      allocationMgr_(&allocationManager),
      pipelineMgr_(&pipelineManager),
      swapChain_(&swapChainManager),
      commandPool_(commandPool) {}

FrameResourceManager::~FrameResourceManager() {
  destroy();
  allocationMgr_->destroyBuffer(fallbackTileGridBuffer_);
  allocationMgr_->destroyBuffer(fallbackExposureStateBuffer_);
  if (gBufferSampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(device_->device(), gBufferSampler_, nullptr);
    gBufferSampler_ = VK_NULL_HANDLE;
  }
}

// -----------------------------------------------------------------------
// Descriptor set layouts
// -----------------------------------------------------------------------
void FrameResourceManager::createDescriptorSetLayouts() {
  if (lightingLayout_ == VK_NULL_HANDLE) {
    const std::array<VkDescriptorSetLayoutBinding, 18> b = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_SAMPLER,           1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        // IBL bindings (10-14)
        {10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // irradiance cubemap
        {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // prefiltered specular cubemap
        {12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // BRDF LUT
        {13, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // env sampler
        {14, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // BRDF LUT sampler
        // AO bindings (15-16)
        {15, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // AO texture
        {16, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // AO sampler
        {17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // specular/F0 G-buffer
    }};
    const std::vector<VkDescriptorBindingFlags> flags(b.size(), 0);
    lightingLayout_ = pipelineMgr_->createDescriptorSetLayout(
        {b.begin(), b.end()}, flags);
  }

  if (postProcessLayout_ == VK_NULL_HANDLE) {
    const std::array<VkDescriptorSetLayoutBinding, 14> b = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // bloom texture
        {8, VK_DESCRIPTOR_TYPE_SAMPLER,       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // bloom sampler
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // tile grid SSBO
        {10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // camera UBO
        {11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // shadow UBO
        {12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // shadow atlas
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // exposure state
    }};
    const std::vector<VkDescriptorBindingFlags> flags(b.size(), 0);
    postProcessLayout_ = pipelineMgr_->createDescriptorSetLayout(
        {b.begin(), b.end()}, flags);
  }

  if (oitLayout_ == VK_NULL_HANDLE) {
    const std::array<VkDescriptorSetLayoutBinding, 4> b = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(b.size(), 0);
    oitLayout_ = pipelineMgr_->createDescriptorSetLayout(
        {b.begin(), b.end()}, flags);
  }
}

// -----------------------------------------------------------------------
void FrameResourceManager::createGBufferSampler() {
  if (gBufferSampler_ != VK_NULL_HANDLE) return;

  VkSamplerCreateInfo info{};
  info.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter        = VK_FILTER_LINEAR;
  info.minFilter        = VK_FILTER_LINEAR;
  info.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  info.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.minLod           = 0.0f;
  info.maxLod           = 0.0f;
  info.maxAnisotropy    = 1.0f;
  info.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

  if (vkCreateSampler(device_->device(), &info, nullptr, &gBufferSampler_) != VK_SUCCESS)
    throw std::runtime_error("failed to create GBuffer sampler");
}

// -----------------------------------------------------------------------
void FrameResourceManager::validateOitFormatSupport() const {
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(),
                                      formats_.oitHeadPointer, &props);
  constexpr VkFormatFeatureFlags kRequiredOitFeatures =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((props.optimalTilingFeatures & kRequiredOitFeatures) !=
      kRequiredOitFeatures)
    throw std::runtime_error(
        "GPU does not support R32_UINT storage/transfer images for exact OIT");
}

// -----------------------------------------------------------------------
uint32_t FrameResourceManager::computeOitNodeCapacity() const {
  const VkExtent2D ext = swapChain_->extent();
  const uint64_t   px  = static_cast<uint64_t>(ext.width) *
                         static_cast<uint64_t>(ext.height);
  const uint64_t desired =
      std::min<uint64_t>(std::max<uint64_t>(1, px * kOitAvgNodesPerPixel),
                         static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
  return std::max<uint32_t>(static_cast<uint32_t>(desired),
                            oitNodeCapacityFloor_);
}

// -----------------------------------------------------------------------
bool FrameResourceManager::growOitPoolIfNeeded(uint32_t imageIndex) {
  if (imageIndex >= frames_.size()) return false;
  auto& frame = frames_[imageIndex];
  if (frame.oitCounterBuffer.buffer == VK_NULL_HANDLE) return false;

  auto* mapped = static_cast<const uint32_t*>(
      frame.oitCounterBuffer.allocation_info.pMappedData);
  if (!mapped) return false;

  if (vmaInvalidateAllocation(allocationMgr_->memoryManager()->allocator(),
                               frame.oitCounterBuffer.allocation, 0,
                               sizeof(uint32_t)) != VK_SUCCESS)
    throw std::runtime_error("failed to invalidate OIT counter buffer");

  const uint32_t required = mapped[0];
  if (required <= frame.oitNodeCapacity) return false;

  const uint64_t doubledCapacity =
      static_cast<uint64_t>(frame.oitNodeCapacity) * 2ull;
  const uint64_t nextCapacity =
      std::max<uint64_t>(required, doubledCapacity);
  oitNodeCapacityFloor_ = static_cast<uint32_t>(std::min<uint64_t>(
      nextCapacity, std::numeric_limits<uint32_t>::max()));
  return true;
}

// -----------------------------------------------------------------------
// create / destroy
// -----------------------------------------------------------------------
void FrameResourceManager::create(
    const GBufferFormats&                    formats,
    VkRenderPass                             depthPrepassPass,
    VkRenderPass                             gBufferPass,
    VkRenderPass                             lightingPass,
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  destroy();

  formats_         = formats;
  depthPrepassPass_ = depthPrepassPass;
  gBufferPass_      = gBufferPass;
  lightingPass_     = lightingPass;

  validateOitFormatSupport();
  ensureFallbackTileGridBuffer();
  ensureFallbackExposureStateBuffer();

  const uint32_t n = static_cast<uint32_t>(swapChain_->imageCount());
  if (n == 0) return;

  VkDevice dev = device_->device();

  // --- Descriptor pools ---
  {
    std::array<VkDescriptorPoolSize, 3> sizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n * 2},
        {VK_DESCRIPTOR_TYPE_SAMPLER, n * 5},    // gbuf + shadow + env + BRDF LUT + AO
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n * 9}, // 5(gbuf) + 1(shadow) + 1(irrad) + 1(prefilt) + 1(brdfLut) + ... err: 5+1+3+1=10 → use 10
    }};
    // Corrected: samplers=5/frame, sampled_images=5(gbuf)+1(shadow)+3(IBL)+1(AO)=10/frame
    sizes[1].descriptorCount = n * 5;
    sizes[2].descriptorCount = n * 11;
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets       = n;
    ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    ci.pPoolSizes    = sizes.data();
    if (vkCreateDescriptorPool(dev, &ci, nullptr, &lightingPool_) != VK_SUCCESS)
      throw std::runtime_error("failed to create lighting descriptor pool");
  }
  {
    std::array<VkDescriptorPoolSize, 4> sizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLER, n * 2},          // gBuffer sampler + bloom sampler
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n * 8},    // sceneColor + 4 gbuf + depth + bloom + shadow atlas
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n * 2},   // tile grid SSBO + exposure state
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n * 2},   // camera + shadow UBO
    }};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets       = n;
    ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    ci.pPoolSizes    = sizes.data();
    if (vkCreateDescriptorPool(dev, &ci, nullptr, &postProcessPool_) != VK_SUCCESS)
      throw std::runtime_error("failed to create post-process descriptor pool");
  }
  {
    std::array<VkDescriptorPoolSize, 3> sizes = {{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  n},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n},
    }};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets       = n;
    ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    ci.pPoolSizes    = sizes.data();
    if (vkCreateDescriptorPool(dev, &ci, nullptr, &oitPool_) != VK_SUCCESS)
      throw std::runtime_error("failed to create OIT descriptor pool");
  }

  // --- Allocate descriptor sets ---
  auto allocSets = [&](VkDescriptorPool pool, VkDescriptorSetLayout layout,
                       uint32_t count) {
    std::vector<VkDescriptorSetLayout> layouts(count, layout);
    std::vector<VkDescriptorSet>       sets(count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = pool;
    ai.descriptorSetCount = count;
    ai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, sets.data()) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate descriptor sets");
    return sets;
  };

  auto lightSets = allocSets(lightingPool_,    lightingLayout_,    n);
  auto postSets  = allocSets(postProcessPool_, postProcessLayout_, n);
  auto oitSets   = allocSets(oitPool_,         oitLayout_,         n);

  // --- Per-frame attachments, buffers, framebuffers ---
  frames_.resize(n);
  const VkExtent2D ext = swapChain_->extent();

  for (uint32_t i = 0; i < n; ++i) {
    auto& f = frames_[i];
    f.lightingDescriptorSet    = lightSets[i];
    f.postProcessDescriptorSet = postSets[i];
    f.oitDescriptorSet         = oitSets[i];

    f.albedo   = createAttachment(formats_.albedo,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    f.normal   = createAttachment(formats_.normal,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    f.material = createAttachment(formats_.material,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    f.emissive = createAttachment(formats_.emissive,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    f.specular = createAttachment(formats_.specular,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    f.sceneColor = createAttachment(formats_.sceneColor,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    f.oitHeadPointers = createAttachment(formats_.oitHeadPointer,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    transitionToGeneral(f.oitHeadPointers.image, VK_IMAGE_ASPECT_COLOR_BIT);

    f.oitNodeCapacity = computeOitNodeCapacity();
    f.oitNodeBuffer = allocationMgr_->createBuffer(
        sizeof(OitNode) * f.oitNodeCapacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    f.oitCounterBuffer = allocationMgr_->createBuffer(
        sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    f.oitMetadataBuffer = allocationMgr_->createBuffer(
        sizeof(OitMetadata),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    f.depthStencil = createAttachment(formats_.depthStencil,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

    // Create a depth-only image view for shader sampling.
    {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image    = f.depthStencil.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format   = formats_.depthStencil;
      vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(dev, &vi, nullptr, &f.depthSamplingView) != VK_SUCCESS)
        throw std::runtime_error("failed to create depth sampling view");
    }

    writeOitMetadata(f);

    // Depth prepass framebuffer
    {
      VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fbi.renderPass      = depthPrepassPass_;
      fbi.attachmentCount = 1;
      fbi.pAttachments    = &f.depthStencil.view;
      fbi.width           = ext.width;
      fbi.height          = ext.height;
      fbi.layers          = 1;
      if (vkCreateFramebuffer(dev, &fbi, nullptr, &f.depthPrepassFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("failed to create depth prepass framebuffer");
    }

    // GBuffer framebuffer
    {
      std::array<VkImageView, 6> views = {f.albedo.view, f.normal.view,
                                          f.material.view, f.emissive.view,
                                          f.specular.view, f.depthStencil.view};
      VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fbi.renderPass      = gBufferPass_;
      fbi.attachmentCount = static_cast<uint32_t>(views.size());
      fbi.pAttachments    = views.data();
      fbi.width           = ext.width;
      fbi.height          = ext.height;
      fbi.layers          = 1;
      if (vkCreateFramebuffer(dev, &fbi, nullptr, &f.gBufferFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("failed to create GBuffer framebuffer");
    }

    // Lighting framebuffer
    {
      std::array<VkImageView, 2> views = {f.sceneColor.view, f.depthStencil.view};
      VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fbi.renderPass      = lightingPass_;
      fbi.attachmentCount = static_cast<uint32_t>(views.size());
      fbi.pAttachments    = views.data();
      fbi.width           = ext.width;
      fbi.height          = ext.height;
      fbi.layers          = 1;
      if (vkCreateFramebuffer(dev, &fbi, nullptr, &f.lightingFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("failed to create lighting framebuffer");
    }
  }

  updateDescriptorSets(cameraBuffers, objectBuffer);
}

// -----------------------------------------------------------------------
void FrameResourceManager::destroy() {
  VkDevice dev = device_->device();

  for (auto& f : frames_) {
    auto destroyFB = [&](VkFramebuffer& fb) {
      if (fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(dev, fb, nullptr); fb = VK_NULL_HANDLE; }
    };
    destroyFB(f.depthPrepassFramebuffer);
    destroyFB(f.gBufferFramebuffer);
    destroyFB(f.lightingFramebuffer);

    destroyAttachment(f.albedo);
    destroyAttachment(f.normal);
    destroyAttachment(f.material);
    destroyAttachment(f.emissive);
    destroyAttachment(f.specular);
    if (f.depthSamplingView != VK_NULL_HANDLE) {
      vkDestroyImageView(dev, f.depthSamplingView, nullptr);
      f.depthSamplingView = VK_NULL_HANDLE;
    }
    destroyAttachment(f.depthStencil);
    destroyAttachment(f.sceneColor);
    destroyAttachment(f.oitHeadPointers);

    allocationMgr_->destroyBuffer(f.oitNodeBuffer);
    allocationMgr_->destroyBuffer(f.oitCounterBuffer);
    allocationMgr_->destroyBuffer(f.oitMetadataBuffer);

    f.lightingDescriptorSet    = VK_NULL_HANDLE;
    f.postProcessDescriptorSet = VK_NULL_HANDLE;
    f.oitDescriptorSet         = VK_NULL_HANDLE;
    f.oitNodeCapacity          = 0;
  }
  frames_.clear();
  descriptorUpdateKey_      = {};
  descriptorUpdateKeyValid_ = false;

  auto destroyPool = [&](VkDescriptorPool& p) {
    if (p != VK_NULL_HANDLE) { vkDestroyDescriptorPool(dev, p, nullptr); p = VK_NULL_HANDLE; }
  };
  destroyPool(lightingPool_);
  destroyPool(postProcessPool_);
  destroyPool(oitPool_);
}

const FrameResources* FrameResourceManager::frame(uint32_t imageIndex) const {
  return imageIndex < frames_.size() ? &frames_[imageIndex] : nullptr;
}

FrameResources* FrameResourceManager::frame(uint32_t imageIndex) {
  return imageIndex < frames_.size() ? &frames_[imageIndex] : nullptr;
}

// -----------------------------------------------------------------------
void FrameResourceManager::updateDescriptorSets(
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer,
    VkImageView shadowAtlasView,
    VkSampler   shadowSampler,
    std::span<const container::gpu::AllocatedBuffer> shadowUbos,
    VkImageView irradianceView,
    VkImageView prefilteredView,
    VkImageView brdfLutView,
    VkSampler   envSampler,
    VkSampler   brdfLutSampler,
    VkImageView aoTextureView,
    VkSampler   aoSampler,
    VkImageView bloomTextureView,
    VkSampler   bloomSampler,
    VkBuffer    tileGridBuffer,
    VkDeviceSize tileGridBufferSize,
    VkBuffer    exposureStateBuffer,
    VkDeviceSize exposureStateBufferSize) {
  (void)objectBuffer;  // reserved for future per-frame object buffer binding
  if (cameraBuffers.empty()) return;

  DescriptorUpdateKey nextKey{};
  nextKey.cameraBuffers.reserve(cameraBuffers.size());
  for (const auto& cameraBuffer : cameraBuffers) {
    nextKey.cameraBuffers.push_back(cameraBuffer.buffer);
  }
  nextKey.shadowUboBuffers.reserve(shadowUbos.size());
  for (const auto& shadowUbo : shadowUbos) {
    nextKey.shadowUboBuffers.push_back(shadowUbo.buffer);
  }
  nextKey.shadowAtlasView   = shadowAtlasView;
  nextKey.shadowSampler     = shadowSampler;
  nextKey.irradianceView    = irradianceView;
  nextKey.prefilteredView   = prefilteredView;
  nextKey.brdfLutView       = brdfLutView;
  nextKey.envSampler        = envSampler;
  nextKey.brdfLutSampler    = brdfLutSampler;
  nextKey.aoTextureView     = aoTextureView;
  nextKey.aoSampler         = aoSampler;
  nextKey.bloomTextureView  = bloomTextureView;
  nextKey.bloomSampler      = bloomSampler;
  nextKey.tileGridBuffer    = tileGridBuffer;
  nextKey.tileGridBufferSize = tileGridBufferSize;
  nextKey.exposureStateBuffer = exposureStateBuffer;
  nextKey.exposureStateBufferSize = exposureStateBufferSize;

  if (descriptorUpdateKeyValid_ && nextKey == descriptorUpdateKey_) {
    return;
  }
  descriptorUpdateKey_      = std::move(nextKey);
  descriptorUpdateKeyValid_ = true;

  VkDevice dev = device_->device();

  for (size_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
    auto& f = frames_[frameIndex];
    const auto& cameraBuffer =
        cameraBuffers[std::min(frameIndex, cameraBuffers.size() - 1)];
    if (cameraBuffer.buffer == VK_NULL_HANDLE) continue;

    VkDescriptorBufferInfo camInfo{cameraBuffer.buffer, 0, sizeof(container::gpu::CameraData)};
    VkDescriptorImageInfo  sampInfo{};
    sampInfo.sampler = gBufferSampler_;

    auto imgInfo = [](VkImageView v, VkImageLayout l) {
      VkDescriptorImageInfo i{}; i.imageView = v; i.imageLayout = l; return i;
    };
    auto albedo   = imgInfo(f.albedo.view,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto normal   = imgInfo(f.normal.view,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto material = imgInfo(f.material.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto emissive = imgInfo(f.emissive.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto specular = imgInfo(f.specular.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto depthImg = imgInfo(f.depthSamplingView,
                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL);
    auto scColor  = imgInfo(f.sceneColor.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto depthPostProcess = imgInfo(f.depthSamplingView,
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    // Lighting set
    {
      std::array<VkWriteDescriptorSet, 18> w{};
      auto set = f.lightingDescriptorSet;
      auto buf = [&](int b, VkDescriptorType t, const VkDescriptorBufferInfo* i) {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set; w[b].dstBinding = b;
        w[b].descriptorCount = 1; w[b].descriptorType = t;
        w[b].pBufferInfo = i;
      };
      auto img = [&](int b, VkDescriptorType t, const VkDescriptorImageInfo* i) {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set; w[b].dstBinding = b;
        w[b].descriptorCount = 1; w[b].descriptorType = t;
        w[b].pImageInfo = i;
      };
      buf(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &camInfo);
      img(1, VK_DESCRIPTOR_TYPE_SAMPLER,        &sampInfo);
      img(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &albedo);
      img(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &normal);
      img(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &material);
      img(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &emissive);
      img(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &depthImg);

      // Shadow bindings (7=UBO, 8=atlas, 9=sampler)
      VkDescriptorBufferInfo shadowUboInfo{};
      const auto& shadowUbo = shadowUbos.empty()
                                  ? container::gpu::AllocatedBuffer{}
                                  : shadowUbos[std::min(frameIndex, shadowUbos.size() - 1)];
      if (shadowUbo.buffer != VK_NULL_HANDLE) {
        shadowUboInfo = {shadowUbo.buffer, 0, sizeof(container::gpu::ShadowData)};
      } else {
        shadowUboInfo = camInfo;  // fallback to avoid null descriptor
      }
      buf(7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &shadowUboInfo);

      VkDescriptorImageInfo shadowAtlasInfo{};
      shadowAtlasInfo.imageView = shadowAtlasView;
      shadowAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (shadowAtlasView == VK_NULL_HANDLE) {
        shadowAtlasInfo = depthImg;  // fallback
      }
      img(8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &shadowAtlasInfo);

      VkDescriptorImageInfo shadowSamplerInfo{};
      shadowSamplerInfo.sampler = shadowSampler;
      if (shadowSampler == VK_NULL_HANDLE) {
        shadowSamplerInfo = sampInfo;  // fallback
      }
      img(9, VK_DESCRIPTOR_TYPE_SAMPLER, &shadowSamplerInfo);

      // IBL bindings (10-14)
      VkDescriptorImageInfo irradianceInfo{};
      irradianceInfo.imageView   = irradianceView;
      irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (irradianceView == VK_NULL_HANDLE) {
        irradianceInfo = albedo;  // fallback
      }
      img(10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &irradianceInfo);

      VkDescriptorImageInfo prefilteredInfo{};
      prefilteredInfo.imageView   = prefilteredView;
      prefilteredInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (prefilteredView == VK_NULL_HANDLE) {
        prefilteredInfo = albedo;  // fallback
      }
      img(11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &prefilteredInfo);

      VkDescriptorImageInfo brdfLutInfo{};
      brdfLutInfo.imageView   = brdfLutView;
      brdfLutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (brdfLutView == VK_NULL_HANDLE) {
        brdfLutInfo = albedo;  // fallback
      }
      img(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &brdfLutInfo);

      VkDescriptorImageInfo envSamplerInfo{};
      envSamplerInfo.sampler = envSampler;
      if (envSampler == VK_NULL_HANDLE) {
        envSamplerInfo = sampInfo;  // fallback
      }
      img(13, VK_DESCRIPTOR_TYPE_SAMPLER, &envSamplerInfo);

      VkDescriptorImageInfo brdfLutSamplerInfo{};
      brdfLutSamplerInfo.sampler = brdfLutSampler;
      if (brdfLutSampler == VK_NULL_HANDLE) {
        brdfLutSamplerInfo = sampInfo;  // fallback
      }
      img(14, VK_DESCRIPTOR_TYPE_SAMPLER, &brdfLutSamplerInfo);

      // AO bindings (15-16)
      VkDescriptorImageInfo aoInfo{};
      aoInfo.imageView   = aoTextureView;
      aoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (aoTextureView == VK_NULL_HANDLE) {
        aoInfo = albedo;  // fallback (will produce 0 but shader defaults to 1.0)
      }
      img(15, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &aoInfo);

      VkDescriptorImageInfo aoSamplerInfo{};
      aoSamplerInfo.sampler = aoSampler;
      if (aoSampler == VK_NULL_HANDLE) {
        aoSamplerInfo = sampInfo;  // fallback
      }
      img(16, VK_DESCRIPTOR_TYPE_SAMPLER, &aoSamplerInfo);
      img(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &specular);

      vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }

    // Post-process set
    {
      std::array<VkWriteDescriptorSet, 14> w{};
      auto set = f.postProcessDescriptorSet;
      auto buf = [&](int b, VkDescriptorType t, const VkDescriptorBufferInfo* i) {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set; w[b].dstBinding = b;
        w[b].descriptorCount = 1; w[b].descriptorType = t;
        w[b].pBufferInfo = i;
      };
      auto img = [&](int b, VkDescriptorType t, const VkDescriptorImageInfo* i) {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set; w[b].dstBinding = b;
        w[b].descriptorCount = 1; w[b].descriptorType = t;
        w[b].pImageInfo = i;
      };
      img(0, VK_DESCRIPTOR_TYPE_SAMPLER,        &sampInfo);
      img(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &scColor);
      img(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &albedo);
      img(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &normal);
      img(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &material);
      img(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &emissive);
      img(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  &depthPostProcess);

      // Bloom bindings (7-8)
      VkDescriptorImageInfo bloomInfo{};
      bloomInfo.imageView   = bloomTextureView;
      bloomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (bloomTextureView == VK_NULL_HANDLE) {
        bloomInfo = albedo;  // fallback to avoid null descriptor
      }
      img(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &bloomInfo);

      VkDescriptorImageInfo bloomSampInfo{};
      bloomSampInfo.sampler = bloomSampler;
      if (bloomSampler == VK_NULL_HANDLE) {
        bloomSampInfo = sampInfo;  // fallback
      }
      img(8, VK_DESCRIPTOR_TYPE_SAMPLER, &bloomSampInfo);

      // Tile grid SSBO (binding 9)
      VkDescriptorBufferInfo tileGridInfo{};
      if (tileGridBuffer != VK_NULL_HANDLE && tileGridBufferSize > 0) {
        tileGridInfo = {tileGridBuffer, 0, tileGridBufferSize};
      } else {
        tileGridInfo = {fallbackTileGridBuffer_.buffer, 0, sizeof(container::gpu::TileLightGrid)};
      }
      w[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[9].dstSet = set; w[9].dstBinding = 9;
      w[9].descriptorCount = 1;
      w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[9].pBufferInfo = &tileGridInfo;

      buf(10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &camInfo);

      VkDescriptorBufferInfo postShadowUboInfo{};
      const auto& postShadowUbo = shadowUbos.empty()
                                      ? container::gpu::AllocatedBuffer{}
                                      : shadowUbos[std::min(frameIndex, shadowUbos.size() - 1)];
      if (postShadowUbo.buffer != VK_NULL_HANDLE) {
        postShadowUboInfo = {postShadowUbo.buffer, 0, sizeof(container::gpu::ShadowData)};
      } else {
        postShadowUboInfo = camInfo;
      }
      buf(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &postShadowUboInfo);

      VkDescriptorImageInfo postShadowAtlasInfo{};
      postShadowAtlasInfo.imageView = shadowAtlasView;
      postShadowAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (shadowAtlasView == VK_NULL_HANDLE) {
        postShadowAtlasInfo = depthPostProcess;
      }
      img(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &postShadowAtlasInfo);

      VkDescriptorBufferInfo exposureStateInfo{};
      if (exposureStateBuffer != VK_NULL_HANDLE &&
          exposureStateBufferSize > 0) {
        exposureStateInfo = {exposureStateBuffer, 0,
                             exposureStateBufferSize};
      } else {
        exposureStateInfo = {fallbackExposureStateBuffer_.buffer, 0,
                             sizeof(container::gpu::ExposureStateData)};
      }
      buf(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &exposureStateInfo);

      vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }

    // OIT set
    {
      VkDescriptorImageInfo  headInfo{};
      headInfo.imageView   = f.oitHeadPointers.view;
      headInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkDescriptorBufferInfo nodeInfo{f.oitNodeBuffer.buffer, 0,
                                      sizeof(OitNode) * f.oitNodeCapacity};
      VkDescriptorBufferInfo counterInfo{f.oitCounterBuffer.buffer, 0, sizeof(uint32_t)};
      VkDescriptorBufferInfo metaInfo{f.oitMetadataBuffer.buffer, 0, sizeof(OitMetadata)};

      std::array<VkWriteDescriptorSet, 4> w{};
      auto set = f.oitDescriptorSet;
      w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &headInfo,    nullptr, nullptr};
      w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,      &nodeInfo, nullptr};
      w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,      &counterInfo, nullptr};
      w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              set, 3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr,      &metaInfo, nullptr};
      vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }
  }
}

// -----------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------
AttachmentImage FrameResourceManager::createAttachment(VkFormat fmt,
                                                        VkImageUsageFlags usage,
                                                        VkImageAspectFlags aspect) const {
  AttachmentImage a{};
  a.format = fmt;
  const VkExtent2D ext = swapChain_->extent();

  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType   = VK_IMAGE_TYPE_2D;
  ii.format      = fmt;
  ii.extent      = {ext.width, ext.height, 1};
  ii.mipLevels   = 1;
  ii.arrayLayers = 1;
  ii.samples     = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
  ii.usage       = usage;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  if (vmaCreateImage(allocationMgr_->memoryManager()->allocator(),
                     &ii, &ai, &a.image, &a.allocation, nullptr) != VK_SUCCESS)
    throw std::runtime_error("failed to create GBuffer attachment image");

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image    = a.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format   = fmt;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};

  if (vkCreateImageView(device_->device(), &vi, nullptr, &a.view) != VK_SUCCESS) {
    vmaDestroyImage(allocationMgr_->memoryManager()->allocator(),
                    a.image, a.allocation);
    throw std::runtime_error("failed to create GBuffer attachment view");
  }
  return a;
}

void FrameResourceManager::destroyAttachment(AttachmentImage& a) const {
  if (a.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device_->device(), a.view, nullptr);
    a.view = VK_NULL_HANDLE;
  }
  if (a.image != VK_NULL_HANDLE && a.allocation != nullptr) {
    vmaDestroyImage(allocationMgr_->memoryManager()->allocator(),
                    a.image, a.allocation);
  }
  a = {};
}

void FrameResourceManager::transitionToGeneral(VkImage image,
                                                VkImageAspectFlags mask) const {
  VkCommandBuffer cmd = beginImmediate();

  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = image;
  b.subresourceRange    = {mask, 0, 1, 0, 1};
  b.srcAccessMask       = 0;
  b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);

  endImmediate(cmd);
}

void FrameResourceManager::writeOitMetadata(FrameResources& frame) const {
  if (frame.oitMetadataBuffer.buffer == VK_NULL_HANDLE) return;
  const VkExtent2D ext = swapChain_->extent();
  const OitMetadata meta{frame.oitNodeCapacity, ext.width, ext.height, 0u};
  void* mapped = frame.oitMetadataBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (!mapped) {
    if (vmaMapMemory(allocationMgr_->memoryManager()->allocator(),
                     frame.oitMetadataBuffer.allocation, &mapped) != VK_SUCCESS)
      throw std::runtime_error("failed to map OIT metadata buffer");
    mappedHere = true;
  }
  std::memcpy(mapped, &meta, sizeof(meta));
  if (vmaFlushAllocation(allocationMgr_->memoryManager()->allocator(),
                         frame.oitMetadataBuffer.allocation, 0,
                         sizeof(meta)) != VK_SUCCESS) {
    if (mappedHere)
      vmaUnmapMemory(allocationMgr_->memoryManager()->allocator(),
                     frame.oitMetadataBuffer.allocation);
    throw std::runtime_error("failed to flush OIT metadata buffer");
  }
  if (mappedHere)
    vmaUnmapMemory(allocationMgr_->memoryManager()->allocator(),
                   frame.oitMetadataBuffer.allocation);
}

void FrameResourceManager::ensureFallbackTileGridBuffer() {
  if (fallbackTileGridBuffer_.buffer != VK_NULL_HANDLE) return;

  fallbackTileGridBuffer_ = allocationMgr_->createBuffer(
      sizeof(container::gpu::TileLightGrid),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  const container::gpu::TileLightGrid fallbackTile{};
  void* mapped = fallbackTileGridBuffer_.allocation_info.pMappedData;
  bool mappedHere = false;
  if (!mapped) {
    if (vmaMapMemory(allocationMgr_->memoryManager()->allocator(),
                     fallbackTileGridBuffer_.allocation, &mapped) != VK_SUCCESS)
      throw std::runtime_error("failed to map fallback tile grid buffer");
    mappedHere = true;
  }
  std::memcpy(mapped, &fallbackTile, sizeof(fallbackTile));
  if (vmaFlushAllocation(allocationMgr_->memoryManager()->allocator(),
                         fallbackTileGridBuffer_.allocation, 0,
                         sizeof(fallbackTile)) != VK_SUCCESS) {
    if (mappedHere)
      vmaUnmapMemory(allocationMgr_->memoryManager()->allocator(),
                     fallbackTileGridBuffer_.allocation);
    throw std::runtime_error("failed to flush fallback tile grid buffer");
  }
  if (mappedHere)
    vmaUnmapMemory(allocationMgr_->memoryManager()->allocator(),
                   fallbackTileGridBuffer_.allocation);
}

void FrameResourceManager::ensureFallbackExposureStateBuffer() {
  if (fallbackExposureStateBuffer_.buffer != VK_NULL_HANDLE) return;

  fallbackExposureStateBuffer_ = allocationMgr_->createBuffer(
      sizeof(container::gpu::ExposureStateData),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  container::gpu::ExposureStateData fallbackExposure{};
  fallbackExposure.exposure = 0.25f;
  fallbackExposure.averageLuminance = 0.18f;
  fallbackExposure.targetExposure = 0.25f;
  fallbackExposure.initialized = 0.0f;
  void* mapped = fallbackExposureStateBuffer_.allocation_info.pMappedData;
  bool mappedHere = false;
  if (!mapped) {
    if (vmaMapMemory(allocationMgr_->memoryManager()->allocator(),
                     fallbackExposureStateBuffer_.allocation,
                     &mapped) != VK_SUCCESS)
      throw std::runtime_error("failed to map fallback exposure state buffer");
    mappedHere = true;
  }
  std::memcpy(mapped, &fallbackExposure, sizeof(fallbackExposure));
  if (vmaFlushAllocation(allocationMgr_->memoryManager()->allocator(),
                         fallbackExposureStateBuffer_.allocation, 0,
                         sizeof(fallbackExposure)) != VK_SUCCESS) {
    if (mappedHere)
      vmaUnmapMemory(allocationMgr_->memoryManager()->allocator(),
                     fallbackExposureStateBuffer_.allocation);
    throw std::runtime_error("failed to flush fallback exposure state buffer");
  }
  if (mappedHere)
    vmaUnmapMemory(allocationMgr_->memoryManager()->allocator(),
                   fallbackExposureStateBuffer_.allocation);
}

VkCommandBuffer FrameResourceManager::beginImmediate() const {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool        = commandPool_;
  ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;

  VkCommandBuffer cmd{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(device_->device(), &ai, &cmd) != VK_SUCCESS)
    throw std::runtime_error("failed to allocate immediate command buffer");

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_->device(), commandPool_, 1, &cmd);
    throw std::runtime_error("failed to begin immediate command buffer");
  }
  return cmd;
}

void FrameResourceManager::endImmediate(VkCommandBuffer cmd) const {
  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_->device(), commandPool_, 1, &cmd);
    throw std::runtime_error("failed to end immediate command buffer");
  }
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cmd;
  if (vkQueueSubmit(device_->graphicsQueue(), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_->device(), commandPool_, 1, &cmd);
    throw std::runtime_error("failed to submit immediate command buffer");
  }
  vkQueueWaitIdle(device_->graphicsQueue());
  vkFreeCommandBuffers(device_->device(), commandPool_, 1, &cmd);
}

}  // namespace container::renderer
