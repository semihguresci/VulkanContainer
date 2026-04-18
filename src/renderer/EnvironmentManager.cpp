#include "Container/renderer/EnvironmentManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <vector>

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
// Destroy the current irradiance/prefiltered cubemaps so they can be rebuilt
// ---------------------------------------------------------------------------

void EnvironmentManager::destroyEnvironmentCubemaps() {
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
  destroyView(irradianceCubeView_);
  destroyImage(irradianceCubeImage_, irradianceCubeAlloc_);
  destroyView(prefilteredCubeView_);
  destroyImage(prefilteredCubeImage_, prefilteredCubeAlloc_);
}

// ---------------------------------------------------------------------------
// HDR environment loading: equirect EXR -> cubemap -> irradiance + prefilter
// ---------------------------------------------------------------------------

namespace {

struct Face2Push { uint32_t faceIndex; uint32_t faceSize; };
struct PrefilterPush { uint32_t faceIndex; uint32_t faceSize; float roughness; uint32_t pad0; };

VkShaderModule loadComputeModule(VkDevice dev,
                                  const std::filesystem::path& spv) {
  const auto data = container::util::readFile(spv);
  return container::gpu::createShaderModule(dev, data);
}

}  // namespace

bool EnvironmentManager::loadHdrEnvironment(
    const std::filesystem::path& shaderDir,
    const std::filesystem::path& hdrPath) {
  VkDevice     dev       = device_->device();
  VmaAllocator allocator = allocationManager_.memoryManager()->allocator();

  // ---- 1. Load EXR -----------------------------------------------------
  if (!std::filesystem::exists(hdrPath)) {
    std::println(stderr, "[HDR] File not found: {}", hdrPath.string());
    return false;
  }

  float*      rgba = nullptr;
  int         exrW = 0, exrH = 0;
  const char* exrErr = nullptr;
  if (LoadEXR(&rgba, &exrW, &exrH, hdrPath.string().c_str(), &exrErr) !=
      TINYEXR_SUCCESS) {
    std::println(stderr, "[HDR] LoadEXR failed: {} ({})",
                 hdrPath.string(), exrErr ? exrErr : "unknown");
    if (exrErr) FreeEXRErrorMessage(exrErr);
    return false;
  }
  if (!rgba || exrW <= 0 || exrH <= 0) {
    std::println(stderr, "[HDR] Invalid EXR dimensions");
    if (rgba) free(rgba);
    return false;
  }

  const VkDeviceSize equirectBytes =
      static_cast<VkDeviceSize>(exrW) * static_cast<VkDeviceSize>(exrH) *
      4ull * sizeof(float);

  // Normalize HDR intensity so the IBL energy sits in a range comparable to
  // the 1.0 placeholder cubemap. Outdoor EXRs frequently have sun pixels at
  // 50-500 nits while the average sky is 1-5; feeding those raw numbers into
  // the split-sum integral produces washed-out diffuse and mirror-bright
  // specular IBL on any low-roughness material.
  //
  // Pass 1: clamp per-component firefly pixels (sun disc) to avoid blowing
  //         up the specular prefilter.
  // Pass 2: scale so the MEAN luminance matches kTargetMeanLuminance, which
  //         matches the unit-white placeholder's contribution.
  {
    constexpr float kFireflyClamp       = 50.0f;   // cap each channel
    constexpr float kTargetMeanLuminance = 0.5f;   // roughly matches old placeholder

    const size_t pixelCount =
        static_cast<size_t>(exrW) * static_cast<size_t>(exrH);
    double luminanceSum = 0.0;
    for (size_t i = 0; i < pixelCount; ++i) {
      float* p = rgba + i * 4;
      p[0] = std::min(std::max(p[0], 0.0f), kFireflyClamp);
      p[1] = std::min(std::max(p[1], 0.0f), kFireflyClamp);
      p[2] = std::min(std::max(p[2], 0.0f), kFireflyClamp);
      const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
      luminanceSum += static_cast<double>(lum);
    }

    const double meanLuminance =
        pixelCount > 0 ? luminanceSum / static_cast<double>(pixelCount) : 0.0;
    float scale = 1.0f;
    if (meanLuminance > 1e-4) {
      scale = static_cast<float>(kTargetMeanLuminance / meanLuminance);
    }
    for (size_t i = 0; i < pixelCount; ++i) {
      float* p = rgba + i * 4;
      p[0] *= scale;
      p[1] *= scale;
      p[2] *= scale;
    }

    std::println(
        "[HDR] Normalized: meanLum={:.3f} -> scale={:.4f} (target={:.2f})",
        meanLuminance, scale, kTargetMeanLuminance);
  }

  std::println("[HDR] Loaded {} ({}x{})", hdrPath.string(), exrW, exrH);

  // ---- 2. Upload equirect to a sampled texture -------------------------
  container::gpu::AllocatedBuffer staging =
      allocationManager_.createBuffer(
          equirectBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
  if (staging.allocation_info.pMappedData == nullptr) {
    std::println(stderr, "[HDR] Staging buffer not mapped");
    free(rgba);
    allocationManager_.destroyBuffer(staging);
    return false;
  }
  std::memcpy(staging.allocation_info.pMappedData, rgba,
              static_cast<size_t>(equirectBytes));
  free(rgba);
  rgba = nullptr;

  VkImage       equirectImage = VK_NULL_HANDLE;
  VmaAllocation equirectAlloc = nullptr;
  VkImageView   equirectView  = VK_NULL_HANDLE;
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent      = {static_cast<uint32_t>(exrW),
                      static_cast<uint32_t>(exrH), 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator, &ii, &ai, &equirectImage,
                        &equirectAlloc, nullptr) != VK_SUCCESS) {
      std::println(stderr, "[HDR] Failed to create equirect image");
      allocationManager_.destroyBuffer(staging);
      return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = equirectImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vi, nullptr, &equirectView);
  }

  // ---- 3. Create the three cubemap targets ----------------------------
  constexpr uint32_t kEnvSize        = 512;
  constexpr uint32_t kIrradianceSize = 32;
  constexpr uint32_t kPrefilterSize  = 128;
  constexpr uint32_t kPrefilterMips  = 5;

  auto createCube = [&](uint32_t size, uint32_t mips,
                        VkImage& image, VmaAllocation& alloc,
                        VkImageView& cubeView) -> bool {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent      = {size, size, 1};
    ii.mipLevels   = mips;
    ii.arrayLayers = 6;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator, &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS)
      return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    return vkCreateImageView(dev, &vi, nullptr, &cubeView) == VK_SUCCESS;
  };

  // Replace existing placeholder cubemaps.
  destroyEnvironmentCubemaps();

  VkImage       envImage = VK_NULL_HANDLE;
  VmaAllocation envAlloc = nullptr;
  VkImageView   envCubeView = VK_NULL_HANDLE;
  if (!createCube(kEnvSize, 1, envImage, envAlloc, envCubeView) ||
      !createCube(kIrradianceSize, 1, irradianceCubeImage_,
                  irradianceCubeAlloc_, irradianceCubeView_) ||
      !createCube(kPrefilterSize, kPrefilterMips, prefilteredCubeImage_,
                  prefilteredCubeAlloc_, prefilteredCubeView_)) {
    std::println(stderr, "[HDR] Failed to create cubemap images");
    return false;
  }

  auto makeArrayView = [&](VkImage img, uint32_t mip) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(dev, &vi, nullptr, &v);
    return v;
  };

  VkImageView envStorageView        = makeArrayView(envImage, 0);
  VkImageView irradianceStorageView = makeArrayView(irradianceCubeImage_, 0);
  std::array<VkImageView, kPrefilterMips> prefilterStorageViews{};
  for (uint32_t m = 0; m < kPrefilterMips; ++m)
    prefilterStorageViews[m] = makeArrayView(prefilteredCubeImage_, m);

  // ---- 4. Build the three compute pipelines ---------------------------
  // Descriptor layout shared between the three passes:
  //   binding 0: sampled input image (Texture2D or TextureCube)
  //   binding 1: sampler
  //   binding 2: RWTexture2DArray output (storage image)
  const std::array<VkDescriptorSetLayoutBinding, 3> bindingsTmpl = {{
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_SAMPLER,        1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  const std::vector<VkDescriptorBindingFlags> flags(bindingsTmpl.size(), 0);
  VkDescriptorSetLayout setLayout = pipelineManager_.createDescriptorSetLayout(
      {bindingsTmpl.begin(), bindingsTmpl.end()}, flags);

  auto buildPipeline = [&](const std::filesystem::path& spv,
                            size_t pcSize,
                            VkPipelineLayout& outLayout) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size       = static_cast<uint32_t>(pcSize);
    outLayout = pipelineManager_.createPipelineLayout({setLayout}, {pc});
    VkShaderModule module = loadComputeModule(dev, spv);
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "computeMain";
    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stage;
    ci.layout = outLayout;
    VkPipeline p = pipelineManager_.createComputePipeline(ci, spv.filename().string());
    vkDestroyShaderModule(dev, module, nullptr);
    return p;
  };

  VkPipelineLayout equirectLayout  = VK_NULL_HANDLE;
  VkPipelineLayout irradianceLayout = VK_NULL_HANDLE;
  VkPipelineLayout prefilterLayout = VK_NULL_HANDLE;
  VkPipeline equirectPipe  = buildPipeline(
      shaderDir / "spv_shaders" / "equirect_to_cubemap.comp.spv",
      sizeof(Face2Push), equirectLayout);
  VkPipeline irradiancePipe = buildPipeline(
      shaderDir / "spv_shaders" / "irradiance_convolution.comp.spv",
      sizeof(Face2Push), irradianceLayout);
  VkPipeline prefilterPipe = buildPipeline(
      shaderDir / "spv_shaders" / "prefilter_specular.comp.spv",
      sizeof(PrefilterPush), prefilterLayout);

  // Descriptor pool: 7 sets total (1 equirect + 1 irradiance + 5 prefilter mips).
  constexpr uint32_t kTotalSets = 1 + 1 + kPrefilterMips;
  VkDescriptorPool pool = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTotalSets},
       {VK_DESCRIPTOR_TYPE_SAMPLER,        kTotalSets},
       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  kTotalSets}},
      kTotalSets, 0);

  auto allocSet = [&]() {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &setLayout;
    VkDescriptorSet s = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev, &ai, &s);
    return s;
  };
  VkDescriptorSet equirectSet   = allocSet();
  VkDescriptorSet irradianceSet = allocSet();
  std::array<VkDescriptorSet, kPrefilterMips> prefilterSets{};
  for (auto& s : prefilterSets) s = allocSet();

  auto writeSet = [&](VkDescriptorSet set, VkImageView sampledView,
                      VkImageLayout sampledLayout, VkImageView storageView) {
    VkDescriptorImageInfo sampledInfo{};
    sampledInfo.imageView   = sampledView;
    sampledInfo.imageLayout = sampledLayout;
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = envSampler_;
    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView   = storageView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    std::array<VkWriteDescriptorSet, 3> w{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &sampledInfo;
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &samplerInfo;
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[2].dstSet = set; w[2].dstBinding = 2; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo = &storageInfo;
    vkUpdateDescriptorSets(dev, 3, w.data(), 0, nullptr);
  };

  writeSet(equirectSet,   equirectView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, envStorageView);
  writeSet(irradianceSet, envCubeView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, irradianceStorageView);
  for (uint32_t m = 0; m < kPrefilterMips; ++m)
    writeSet(prefilterSets[m], envCubeView,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             prefilterStorageViews[m]);

  // ---- 5. Record everything on a single-use command buffer ------------
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
  }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  auto barrier = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                      VkAccessFlags srcA, VkAccessFlags dstA,
                      VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
                      uint32_t layerCount, uint32_t levelCount) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levelCount, 0, layerCount};
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
  };

  // Equirect upload: UNDEFINED -> TRANSFER_DST -> copy -> SHADER_READ_ONLY
  barrier(equirectImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, 1);
  {
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {static_cast<uint32_t>(exrW), static_cast<uint32_t>(exrH), 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, equirectImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  }
  barrier(equirectImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 1);

  // Transition cubemap targets to GENERAL for storage writes.
  barrier(envImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 6, 1);
  barrier(irradianceCubeImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 6, 1);
  barrier(prefilteredCubeImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          6, kPrefilterMips);

  // -- Equirect -> Env cubemap (dispatch per face) --
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, equirectPipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, equirectLayout,
                          0, 1, &equirectSet, 0, nullptr);
  for (uint32_t face = 0; face < 6; ++face) {
    Face2Push push{face, kEnvSize};
    vkCmdPushConstants(cmd, equirectLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const uint32_t g = (kEnvSize + 15) / 16;
    vkCmdDispatch(cmd, g, g, 1);
  }

  // Env cube: GENERAL -> SHADER_READ_ONLY for sampling by filters.
  barrier(envImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 6, 1);

  // -- Env -> Irradiance --
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiancePipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradianceLayout,
                          0, 1, &irradianceSet, 0, nullptr);
  for (uint32_t face = 0; face < 6; ++face) {
    Face2Push push{face, kIrradianceSize};
    vkCmdPushConstants(cmd, irradianceLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const uint32_t g = (kIrradianceSize + 7) / 8;
    vkCmdDispatch(cmd, g, g, 1);
  }

  // -- Env -> Prefiltered specular (per mip, per face) --
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilterPipe);
  for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilterLayout,
                            0, 1, &prefilterSets[mip], 0, nullptr);
    const uint32_t mipSize = std::max(1u, kPrefilterSize >> mip);
    const float roughness  = (kPrefilterMips <= 1) ? 0.0f :
        static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    for (uint32_t face = 0; face < 6; ++face) {
      PrefilterPush push{face, mipSize, roughness, 0};
      vkCmdPushConstants(cmd, prefilterLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                         0, sizeof(push), &push);
      const uint32_t g = (mipSize + 15) / 16;
      vkCmdDispatch(cmd, g, g, 1);
    }
  }

  // Final transitions to SHADER_READ_ONLY for the fragment lighting pass.
  barrier(irradianceCubeImage_, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 6, 1);
  barrier(prefilteredCubeImage_, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          6, kPrefilterMips);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cmd;
  vkQueueSubmit(device_->graphicsQueue(), 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(device_->graphicsQueue());
  vkFreeCommandBuffers(dev, commandPool_, 1, &cmd);

  // ---- 6. Cleanup transient resources ---------------------------------
  for (auto v : prefilterStorageViews)
    if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
  if (irradianceStorageView != VK_NULL_HANDLE)
    vkDestroyImageView(dev, irradianceStorageView, nullptr);
  if (envStorageView != VK_NULL_HANDLE)
    vkDestroyImageView(dev, envStorageView, nullptr);
  if (envCubeView != VK_NULL_HANDLE)
    vkDestroyImageView(dev, envCubeView, nullptr);
  if (envImage != VK_NULL_HANDLE && envAlloc != nullptr)
    vmaDestroyImage(allocator, envImage, envAlloc);

  if (equirectView != VK_NULL_HANDLE)
    vkDestroyImageView(dev, equirectView, nullptr);
  if (equirectImage != VK_NULL_HANDLE && equirectAlloc != nullptr)
    vmaDestroyImage(allocator, equirectImage, equirectAlloc);

  allocationManager_.destroyBuffer(staging);

  std::println("[HDR] IBL generation complete");
  return true;
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
    // TRANSFER_DST so we can vkCmdClearColorImage() to a non-zero gray below.
    // Without a real HDR IBL loaded, sampling undefined memory produces zero
    // and the deferred lighting's ambient term collapses to black in shadow.
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

  auto transitionCube = [&](VkImage img, VkImageLayout oldLayout,
                            VkImageLayout newLayout,
                            VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                            VkPipelineStageFlags srcStage,
                            VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout     = oldLayout;
    barrier.newLayout     = newLayout;
    barrier.image         = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  };

  // Transition to TRANSFER_DST_OPTIMAL so we can clear the cubemaps.
  transitionCube(irradianceCubeImage_,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  transitionCube(prefilteredCubeImage_,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Clear both cubemaps to a mid-gray so the deferred lighting ambient term
  // has something meaningful to sample until a real HDR environment is
  // loaded. Values chosen in linear space; the tonemapper will map them to
  // a neutral ambient.
  VkClearColorValue clearColor{};
  clearColor.float32[0] = 0.25f;
  clearColor.float32[1] = 0.25f;
  clearColor.float32[2] = 0.25f;
  clearColor.float32[3] = 1.0f;
  VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  vkCmdClearColorImage(cmd, irradianceCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);
  vkCmdClearColorImage(cmd, prefilteredCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);

  // Transition to SHADER_READ_ONLY for sampling in the lighting pass.
  transitionCube(irradianceCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  transitionCube(prefilteredCubeImage_,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

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
