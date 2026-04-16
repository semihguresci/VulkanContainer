#include "Container/renderer/EnvironmentManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace container::renderer {

EnvironmentManager::EnvironmentManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&              pipelineManager,
    VkCommandPool                                  commandPool)
    : device_(std::move(device)),
      allocationManager_(allocationManager),
      pipelineManager_(pipelineManager),
      commandPool_(commandPool) {}

EnvironmentManager::~EnvironmentManager() {
  destroy();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void EnvironmentManager::createResources(const std::filesystem::path& shaderDir) {
  createSamplers();
  createBrdfLut(shaderDir);
  createPlaceholderCubemaps();
}

void EnvironmentManager::createGtaoResources(
    const std::filesystem::path& shaderDir,
    uint32_t fullWidth, uint32_t fullHeight) {
  createGtaoPipelines(shaderDir);
  createGtaoTextures(fullWidth / 2, fullHeight / 2);
}

void EnvironmentManager::recreateGtaoTextures(uint32_t fullWidth,
                                               uint32_t fullHeight) {
  destroyGtaoTextures();
  if (gtaoPipeline_ != VK_NULL_HANDLE) {
    createGtaoTextures(fullWidth / 2, fullHeight / 2);
  }
}

void EnvironmentManager::destroy() {
  VkDevice dev = device_->device();

  destroyGtaoTextures();

  auto destroyView = [&](VkImageView& v) {
    if (v != VK_NULL_HANDLE) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
  };
  auto destroySampler = [&](VkSampler& s) {
    if (s != VK_NULL_HANDLE) { vkDestroySampler(dev, s, nullptr); s = VK_NULL_HANDLE; }
  };
  auto destroyImage = [&](VkImage& img, VmaAllocation& alloc) {
    if (img != VK_NULL_HANDLE && alloc != nullptr) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), img, alloc);
    }
    img = VK_NULL_HANDLE; alloc = nullptr;
  };

  destroyView(brdfLutView_);
  destroyImage(brdfLutImage_, brdfLutAlloc_);
  destroySampler(brdfLutSampler_);

  destroyView(irradianceCubeView_);
  destroyImage(irradianceCubeImage_, irradianceCubeAlloc_);

  destroyView(prefilteredCubeView_);
  destroyImage(prefilteredCubeImage_, prefilteredCubeAlloc_);

  destroySampler(envSampler_);
  destroySampler(gtaoSampler_);
}

// ---------------------------------------------------------------------------
// BRDF LUT generation (one-time compute dispatch)
// ---------------------------------------------------------------------------

void EnvironmentManager::createBrdfLut(const std::filesystem::path& shaderDir) {
  VkDevice dev = device_->device();
  constexpr uint32_t kLutSize = 512;

  // Create the LUT image.
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16_SFLOAT;
    ii.extent      = {kLutSize, kLutSize, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &brdfLutImage_, &brdfLutAlloc_, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create BRDF LUT image");
  }

  // Create image view.
  {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = brdfLutImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &brdfLutView_) != VK_SUCCESS)
      throw std::runtime_error("failed to create BRDF LUT view");
  }

  // Create compute pipeline for BRDF LUT generation.
  VkDescriptorSetLayout lutSetLayout = VK_NULL_HANDLE;
  {
    const VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    lutSetLayout = pipelineManager_.createDescriptorSetLayout({binding}, {0});
    brdfLutSetLayout_ = lutSetLayout;
  }

  VkDescriptorPool lutPool = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}}, 1, 0);

  VkDescriptorSet lutSet = VK_NULL_HANDLE;
  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = lutPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &lutSetLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &lutSet) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate BRDF LUT descriptor set");
  }

  VkPipelineLayout lutPipelineLayout = pipelineManager_.createPipelineLayout(
      {lutSetLayout}, {});

  std::filesystem::path spvPath = shaderDir / "spv_shaders" / "brdf_lut.comp.spv";
  const auto spvData = container::util::readFile(spvPath);
  VkShaderModule module = container::gpu::createShaderModule(dev, spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = module;
  stage.pName  = "computeMain";

  VkComputePipelineCreateInfo ci{};
  ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage  = stage;
  ci.layout = lutPipelineLayout;

  VkPipeline lutPipeline = pipelineManager_.createComputePipeline(ci, "brdf_lut");

  vkDestroyShaderModule(dev, module, nullptr);

  // Transition image to GENERAL for compute write.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate BRDF LUT command buffer");
  }

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  // Transition to GENERAL.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = brdfLutImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // Write descriptor.
  {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = brdfLutView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = lutSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }

  // Dispatch.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lutPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lutPipelineLayout,
                          0, 1, &lutSet, 0, nullptr);
  vkCmdDispatch(cmd, (kLutSize + 15) / 16, (kLutSize + 15) / 16, 1);

  // Transition to SHADER_READ_ONLY.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image         = brdfLutImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  vkEndCommandBuffer(cmd);

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;
  vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(device_->graphicsQueue());

  vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
}

// ---------------------------------------------------------------------------
// Placeholder cubemaps (white 1×1 cubemaps — replaced when HDR env loaded)
// ---------------------------------------------------------------------------

void EnvironmentManager::createPlaceholderCubemaps() {
  VkDevice dev = device_->device();

  auto createCube = [&](uint32_t size, VkImage& image, VmaAllocation& alloc,
                        VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent      = {size, size, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 6;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create placeholder cubemap image");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS)
      throw std::runtime_error("failed to create placeholder cubemap view");
  };

  createCube(1, irradianceCubeImage_, irradianceCubeAlloc_, irradianceCubeView_);
  createCube(1, prefilteredCubeImage_, prefilteredCubeAlloc_, prefilteredCubeView_);

  // Transition both to SHADER_READ_ONLY.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
  }
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  auto transitionCube = [&](VkImage img) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image         = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  };

  transitionCube(irradianceCubeImage_);
  transitionCube(prefilteredCubeImage_);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;
  vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(device_->graphicsQueue());
  vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);
}

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

void EnvironmentManager::createSamplers() {
  VkDevice dev = device_->device();

  // BRDF LUT sampler (clamp-to-edge, linear).
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = 0.0f;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &brdfLutSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create BRDF LUT sampler");
  }

  // Environment cubemap sampler (linear, mip-mapped for prefiltered specular).
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = 5.0f;  // for mip levels
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &envSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create environment sampler");
  }

  // GTAO sampler (linear, clamp-to-edge).
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod       = 0.0f;
    si.maxLod       = 0.0f;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &gtaoSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create GTAO sampler");
  }
}

// ---------------------------------------------------------------------------
// GTAO pipelines
// ---------------------------------------------------------------------------

void EnvironmentManager::createGtaoPipelines(const std::filesystem::path& shaderDir) {
  VkDevice dev = device_->device();

  // GTAO compute pipeline.
  {
    const std::array<VkDescriptorSetLayoutBinding, 6> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    gtaoSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);

    struct GtaoPushConstants {
      float    aoRadius;
      float    aoIntensity;
      uint32_t sampleCount;
      uint32_t pad0;
      uint32_t fullWidth;
      uint32_t fullHeight;
      uint32_t pad1;
      uint32_t pad2;
    };

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(GtaoPushConstants);

    gtaoPipelineLayout_ = pipelineManager_.createPipelineLayout(
        {gtaoSetLayout_}, {pcRange});

    std::filesystem::path spvPath = shaderDir / "spv_shaders" / "gtao.comp.spv";
    const auto spvData = container::util::readFile(spvPath);
    VkShaderModule module = container::gpu::createShaderModule(dev, spvData);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "computeMain";

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stage;
    ci.layout = gtaoPipelineLayout_;

    gtaoPipeline_ = pipelineManager_.createComputePipeline(ci, "gtao");
    vkDestroyShaderModule(dev, module, nullptr);
  }

  // GTAO blur pipeline.
  {
    const std::array<VkDescriptorSetLayoutBinding, 5> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    gtaoBlurSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);

    struct BlurPushConstants {
      uint32_t width;
      uint32_t height;
      float    depthThreshold;
      uint32_t pad0;
    };

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(BlurPushConstants);

    gtaoBlurPipelineLayout_ = pipelineManager_.createPipelineLayout(
        {gtaoBlurSetLayout_}, {pcRange});

    std::filesystem::path spvPath = shaderDir / "spv_shaders" / "gtao_blur.comp.spv";
    const auto spvData = container::util::readFile(spvPath);
    VkShaderModule module = container::gpu::createShaderModule(dev, spvData);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "computeMain";

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stage;
    ci.layout = gtaoBlurPipelineLayout_;

    gtaoBlurPipeline_ = pipelineManager_.createComputePipeline(ci, "gtao_blur");
    vkDestroyShaderModule(dev, module, nullptr);
  }

  // Descriptor pools.
  gtaoDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
       {VK_DESCRIPTOR_TYPE_SAMPLER, 2},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
      1, 0);
  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = gtaoDescriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &gtaoSetLayout_;
    if (vkAllocateDescriptorSets(dev, &ai, &gtaoSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate GTAO descriptor set");
  }

  gtaoBlurDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
       {VK_DESCRIPTOR_TYPE_SAMPLER, 2},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
      1, 0);
  {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = gtaoBlurDescriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &gtaoBlurSetLayout_;
    if (vkAllocateDescriptorSets(dev, &ai, &gtaoBlurSet_) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate GTAO blur descriptor set");
  }
}

// ---------------------------------------------------------------------------
// GTAO textures (half-resolution R8)
// ---------------------------------------------------------------------------

void EnvironmentManager::createGtaoTextures(uint32_t halfWidth,
                                             uint32_t halfHeight) {
  VkDevice dev = device_->device();
  gtaoWidth_  = std::max(halfWidth, 1u);
  gtaoHeight_ = std::max(halfHeight, 1u);

  auto createR8 = [&](VkImage& image, VmaAllocation& alloc, VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R8_UNORM;
    ii.extent      = {gtaoWidth_, gtaoHeight_, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create GTAO texture");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS)
      throw std::runtime_error("failed to create GTAO texture view");
  };

  createR8(gtaoImage_, gtaoAlloc_, gtaoView_);
  createR8(gtaoBlurredImage_, gtaoBlurredAlloc_, gtaoBlurredView_);

  // Transition both to GENERAL for compute write.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
  }
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  auto transition = [&](VkImage img) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  };
  transition(gtaoImage_);
  transition(gtaoBlurredImage_);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;
  vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(device_->graphicsQueue());
  vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);

  // Write blur set descriptors (inAO = gtaoView_, outAO = gtaoBlurredView_).
  // The GTAO main set is written per-frame in dispatchGtao.
  {
    VkDescriptorImageInfo aoInInfo{};
    aoInInfo.imageView   = gtaoView_;
    aoInInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo aoSampInfo{};
    aoSampInfo.sampler = gtaoSampler_;

    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = VK_NULL_HANDLE;  // placeholder — updated at dispatch
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo depthSampInfo{};
    depthSampInfo.sampler = gtaoSampler_;

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = gtaoBlurredView_;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 5> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = gtaoBlurSet_; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &aoInInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = gtaoBlurSet_; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &aoSampInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = gtaoBlurSet_; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[2].pImageInfo = &depthInfo;

    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[3].dstSet = gtaoBlurSet_; w[3].dstBinding = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[3].pImageInfo = &depthSampInfo;

    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[4].dstSet = gtaoBlurSet_; w[4].dstBinding = 4;
    w[4].descriptorCount = 1;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[4].pImageInfo = &outInfo;

    // Only write bindings 0, 1, 4 now — 2 and 3 (depth) updated at dispatch time.
    vkUpdateDescriptorSets(dev, 2, w.data(), 0, nullptr);   // bindings 0,1
    vkUpdateDescriptorSets(dev, 1, &w[4], 0, nullptr);      // binding 4
  }
}

void EnvironmentManager::destroyGtaoTextures() {
  VkDevice dev = device_->device();

  auto destroyView = [&](VkImageView& v) {
    if (v != VK_NULL_HANDLE) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
  };
  auto destroyImage = [&](VkImage& img, VmaAllocation& alloc) {
    if (img != VK_NULL_HANDLE && alloc != nullptr) {
      vmaDestroyImage(allocationManager_.memoryManager()->allocator(), img, alloc);
    }
    img = VK_NULL_HANDLE; alloc = nullptr;
  };

  destroyView(gtaoView_);
  destroyImage(gtaoImage_, gtaoAlloc_);
  destroyView(gtaoBlurredView_);
  destroyImage(gtaoBlurredImage_, gtaoBlurredAlloc_);

  gtaoWidth_ = 0;
  gtaoHeight_ = 0;
}

// ---------------------------------------------------------------------------
// Per-frame GTAO dispatch
// ---------------------------------------------------------------------------

void EnvironmentManager::dispatchGtao(
    VkCommandBuffer cmd,
    uint32_t fullWidth, uint32_t fullHeight,
    VkBuffer cameraBuffer, VkDeviceSize cameraBufferSize,
    VkImageView depthView, VkSampler depthSampler,
    VkImageView normalView, VkSampler normalSampler) const {
  if (gtaoPipeline_ == VK_NULL_HANDLE || !aoEnabled_) return;
  if (gtaoView_ == VK_NULL_HANDLE) return;

  VkDevice dev = device_->device();

  // Update GTAO descriptor set with per-frame resources.
  {
    VkDescriptorBufferInfo camInfo{cameraBuffer, 0, cameraBufferSize};
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    VkDescriptorImageInfo depthSampInfo{};
    depthSampInfo.sampler = depthSampler;
    VkDescriptorImageInfo normalInfo{};
    normalInfo.imageView   = normalView;
    normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo normalSampInfo{};
    normalSampInfo.sampler = normalSampler;
    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = gtaoView_;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 6> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = gtaoSet_; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &camInfo;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = gtaoSet_; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &depthInfo;

    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = gtaoSet_; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[2].pImageInfo = &depthSampInfo;

    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[3].dstSet = gtaoSet_; w[3].dstBinding = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[3].pImageInfo = &normalInfo;

    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[4].dstSet = gtaoSet_; w[4].dstBinding = 4;
    w[4].descriptorCount = 1;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[4].pImageInfo = &normalSampInfo;

    w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[5].dstSet = gtaoSet_; w[5].dstBinding = 5;
    w[5].descriptorCount = 1;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[5].pImageInfo = &outInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
  }

  struct GtaoPushConstants {
    float    aoRadius;
    float    aoIntensity;
    uint32_t sampleCount;
    uint32_t pad0;
    uint32_t fullWidth;
    uint32_t fullHeight;
    uint32_t pad1;
    uint32_t pad2;
  };

  GtaoPushConstants pc{};
  pc.aoRadius    = aoRadius_;
  pc.aoIntensity = aoIntensity_;
  pc.sampleCount = aoSampleCount_;
  pc.fullWidth   = fullWidth;
  pc.fullHeight  = fullHeight;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoPipelineLayout_,
                          0, 1, &gtaoSet_, 0, nullptr);
  vkCmdPushConstants(cmd, gtaoPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(GtaoPushConstants), &pc);

  uint32_t dispatchX = (gtaoWidth_ + 7) / 8;
  uint32_t dispatchY = (gtaoHeight_ + 7) / 8;
  vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

  // Barrier: GTAO write → blur read.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = gtaoImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

void EnvironmentManager::dispatchGtaoBlur(VkCommandBuffer cmd) const {
  if (gtaoBlurPipeline_ == VK_NULL_HANDLE || !aoEnabled_) return;
  if (gtaoBlurredView_ == VK_NULL_HANDLE) return;

  // Update blur depth descriptors (bindings 2,3) at dispatch time.
  // Note: for simplicity we reuse the same AO sampler for depth in the blur.
  // The blur set's bindings 0,1,4 were written in createGtaoTextures.

  struct BlurPushConstants {
    uint32_t width;
    uint32_t height;
    float    depthThreshold;
    uint32_t pad0;
  };

  BlurPushConstants pc{};
  pc.width          = gtaoWidth_;
  pc.height         = gtaoHeight_;
  pc.depthThreshold = 0.001f;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurPipelineLayout_,
                          0, 1, &gtaoBlurSet_, 0, nullptr);
  vkCmdPushConstants(cmd, gtaoBlurPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(BlurPushConstants), &pc);

  uint32_t dispatchX = (gtaoWidth_ + 7) / 8;
  uint32_t dispatchY = (gtaoHeight_ + 7) / 8;
  vkCmdDispatch(cmd, dispatchX, dispatchY, 1);

  // Barrier: blur write → fragment read in lighting.
  {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = gtaoBlurredImage_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

}  // namespace container::renderer
