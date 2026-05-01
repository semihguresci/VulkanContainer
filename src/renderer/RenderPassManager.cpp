#include "Container/renderer/RenderPassManager.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace container::renderer {

namespace {

constexpr VkPipelineStageFlags kDepthTestStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

constexpr VkDependencyFlags kFramebufferLocalDependency =
    VK_DEPENDENCY_BY_REGION_BIT;

VkSubpassDependency MakeDependency(uint32_t srcSubpass,
                                   uint32_t dstSubpass,
                                   VkPipelineStageFlags srcStages,
                                   VkPipelineStageFlags dstStages,
                                   VkAccessFlags srcAccess,
                                   VkAccessFlags dstAccess,
                                   VkDependencyFlags flags = 0) {
  VkSubpassDependency dependency{};
  dependency.srcSubpass = srcSubpass;
  dependency.dstSubpass = dstSubpass;
  dependency.srcStageMask = srcStages;
  dependency.dstStageMask = dstStages;
  dependency.srcAccessMask = srcAccess;
  dependency.dstAccessMask = dstAccess;
  dependency.dependencyFlags = flags;
  return dependency;
}

}  // namespace

RenderPassManager::RenderPassManager(
    std::shared_ptr<container::gpu::VulkanDevice> device)
    : device_(std::move(device)) {}

RenderPassManager::~RenderPassManager() { destroy(); }

VkFormat RenderPassManager::findSupportedFormat(
    std::initializer_list<VkFormat> candidates, VkImageTiling tiling,
    VkFormatFeatureFlags features) const {
  for (VkFormat fmt : candidates) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), fmt, &props);
    const VkFormatFeatureFlags tilingFeatures =
        tiling == VK_IMAGE_TILING_LINEAR ? props.linearTilingFeatures
                                         : props.optimalTilingFeatures;
    if ((tilingFeatures & features) == features) return fmt;
  }
  throw std::runtime_error("failed to find a supported Vulkan image format");
}

VkFormat RenderPassManager::findDepthStencilFormat() const {
  return findSupportedFormat(
      {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT},
      VK_IMAGE_TILING_OPTIMAL,
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

void RenderPassManager::create(VkFormat swapchainFormat,
                               VkFormat depthStencilFormat,
                               VkFormat sceneColorFormat,
                               VkFormat albedoFormat,
                               VkFormat normalFormat,
                               VkFormat materialFormat,
                               VkFormat emissiveFormat,
                               VkFormat specularFormat) {
  VkDevice dev = device_->device();

  // ---- Depth Prepass ----
  VkAttachmentDescription ds{};
  ds.format         = depthStencilFormat;
  ds.samples        = VK_SAMPLE_COUNT_1_BIT;
  ds.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  ds.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  ds.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  ds.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ds.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  ds.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference dsRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription depthSubpass{};
  depthSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
  depthSubpass.pDepthStencilAttachment = &dsRef;

  std::array<VkSubpassDependency, 2> depthDeps{};
  depthDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, kDepthTestStages,
      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);
  depthDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      kDepthTestStages, kDepthTestStages,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);

  VkRenderPassCreateInfo rpInfo{};
  rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments    = &ds;
  rpInfo.subpassCount    = 1;
  rpInfo.pSubpasses      = &depthSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(depthDeps.size());
  rpInfo.pDependencies   = depthDeps.data();
  if (vkCreateRenderPass(dev, &rpInfo, nullptr, &passes_.depthPrepass) != VK_SUCCESS)
    throw std::runtime_error("failed to create depth prepass render pass");

  // ---- GBuffer ----
  auto makeColor = [](VkFormat fmt) {
    VkAttachmentDescription a{};
    a.format         = fmt;
    a.samples        = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return a;
  };

  VkAttachmentDescription gbDs = ds;
  gbDs.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
  gbDs.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  gbDs.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  gbDs.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  std::array<VkAttachmentDescription, 6> gbAttachments = {
      makeColor(albedoFormat), makeColor(normalFormat), makeColor(materialFormat),
      makeColor(emissiveFormat), makeColor(specularFormat), gbDs};

  std::array<VkAttachmentReference, 5> colorRefs = {{
      {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
  }};
  VkAttachmentReference gbDsRef{5, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription gbSubpass{};
  gbSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  gbSubpass.colorAttachmentCount    = static_cast<uint32_t>(colorRefs.size());
  gbSubpass.pColorAttachments       = colorRefs.data();
  gbSubpass.pDepthStencilAttachment = &gbDsRef;

  std::array<VkSubpassDependency, 2> gbDeps{};
  // Entry: depth prepass wrote depth, then Hi-Z compute may have read it.
  // Occlusion culling may also have produced indirect commands for this pass.
  gbDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      kDepthTestStages | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  // Exit: GBuffer colors → shader read (lighting pass),
  //       depth → compute shader read (TileCull, GTAO) + attachment read (lighting).
  gbDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT);

  rpInfo.attachmentCount = static_cast<uint32_t>(gbAttachments.size());
  rpInfo.pAttachments    = gbAttachments.data();
  rpInfo.pSubpasses      = &gbSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(gbDeps.size());
  rpInfo.pDependencies   = gbDeps.data();
  if (vkCreateRenderPass(dev, &rpInfo, nullptr, &passes_.gBuffer) != VK_SUCCESS)
    throw std::runtime_error("failed to create GBuffer render pass");

  // ---- Lighting ----
  VkAttachmentDescription lightColor{};
  lightColor.format         = sceneColorFormat;
  lightColor.samples        = VK_SAMPLE_COUNT_1_BIT;
  lightColor.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  lightColor.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  lightColor.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  lightColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  lightColor.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  lightColor.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentDescription lightDs = ds;
  lightDs.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
  // Stencil is cleared per-light-volume via vkCmdClearAttachments; no need to
  // preserve stale values from the GBuffer pass.
  lightDs.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  lightDs.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // After GBuffer, depth is transitioned to DEPTH_READ_ONLY_STENCIL_ATTACHMENT
  // for TileCull/GTAO compute reads; the lighting pass picks it up in that layout.
  lightDs.initialLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  lightDs.finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

  std::array<VkAttachmentDescription, 2> lightAttachments = {lightColor, lightDs};
  VkAttachmentReference lightColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference lightDsRef{1, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription lightSubpass{};
  lightSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  lightSubpass.colorAttachmentCount    = 1;
  lightSubpass.pColorAttachments       = &lightColorRef;
  lightSubpass.pDepthStencilAttachment = &lightDsRef;

  std::array<VkSubpassDependency, 2> lightDeps{};
  // Entry: preceding passes provide GBuffer textures, read-only depth,
  // tile-light grids, AO, and cleared OIT storage for transparent fragments.
  lightDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  lightDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

  rpInfo.attachmentCount = static_cast<uint32_t>(lightAttachments.size());
  rpInfo.pAttachments    = lightAttachments.data();
  rpInfo.pSubpasses      = &lightSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(lightDeps.size());
  rpInfo.pDependencies   = lightDeps.data();
  if (vkCreateRenderPass(dev, &rpInfo, nullptr, &passes_.lighting) != VK_SUCCESS)
    throw std::runtime_error("failed to create lighting render pass");

  // ---- Shadow Depth ----
  // Depth-only render pass for shadow map cascades. Uses the same depth format
  // as the main scene but renders into a 2D array layer per cascade.
  VkAttachmentDescription shadowDs{};
  shadowDs.format         = depthStencilFormat;
  shadowDs.samples        = VK_SAMPLE_COUNT_1_BIT;
  shadowDs.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  shadowDs.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  shadowDs.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  shadowDs.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  shadowDs.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  shadowDs.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference shadowDsRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription shadowSubpass{};
  shadowSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  shadowSubpass.pDepthStencilAttachment = &shadowDsRef;

  std::array<VkSubpassDependency, 2> shadowDeps{};
  shadowDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      kDepthTestStages | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  shadowDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      kDepthTestStages,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT);

  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments    = &shadowDs;
  rpInfo.subpassCount    = 1;
  rpInfo.pSubpasses      = &shadowSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(shadowDeps.size());
  rpInfo.pDependencies   = shadowDeps.data();
  if (vkCreateRenderPass(dev, &rpInfo, nullptr, &passes_.shadow) != VK_SUCCESS)
    throw std::runtime_error("failed to create shadow render pass");

  // ---- Post Process ----
  VkAttachmentDescription swapAttachment{};
  swapAttachment.format         = swapchainFormat;
  swapAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
  swapAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  swapAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  swapAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swapAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swapAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  swapAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference swapRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription postSubpass{};
  postSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
  postSubpass.colorAttachmentCount = 1;
  postSubpass.pColorAttachments    = &swapRef;

  std::array<VkSubpassDependency, 2> postDeps{};
  postDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
  postDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);

  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments    = &swapAttachment;
  rpInfo.pSubpasses      = &postSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(postDeps.size());
  rpInfo.pDependencies   = postDeps.data();
  if (vkCreateRenderPass(dev, &rpInfo, nullptr, &passes_.postProcess) != VK_SUCCESS)
    throw std::runtime_error("failed to create post-process render pass");
}

void RenderPassManager::destroy() {
  VkDevice dev = device_->device();
  auto destroyPass = [&](VkRenderPass& rp) {
    if (rp != VK_NULL_HANDLE) {
      vkDestroyRenderPass(dev, rp, nullptr);
      rp = VK_NULL_HANDLE;
    }
  };
  destroyPass(passes_.depthPrepass);
  destroyPass(passes_.gBuffer);
  destroyPass(passes_.shadow);
  destroyPass(passes_.lighting);
  destroyPass(passes_.postProcess);
}

}  // namespace container::renderer
