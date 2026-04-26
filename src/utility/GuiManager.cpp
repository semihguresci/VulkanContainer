#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

#include "Container/utility/GuiManager.h"

#include "Container/common/CommonGLFW.h"
#include "Container/utility/SceneGraph.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

#include "Container/common/CommonMath.h"


namespace container::ui {

using container::gpu::kMaxDeferredPointLights;
using container::gpu::LightingData;
using container::gpu::LightingSettings;

namespace {

constexpr std::array<const char*, 4> kLightingPresetLabels = {{
    "Sponza", "Interior", "Exterior", "Custom",
}};

LightingSettings LightingPreset(uint32_t preset) {
  LightingSettings settings{};
  settings.preset = preset;
  switch (preset) {
    case 1:
      settings.density = 1.8f;
      settings.radiusScale = 1.25f;
      settings.intensityScale = 1.35f;
      settings.directionalIntensity = 1.15f;
      break;
    case 2:
      settings.density = 0.55f;
      settings.radiusScale = 1.8f;
      settings.intensityScale = 0.65f;
      settings.directionalIntensity = 3.25f;
      break;
    case 0:
    default:
      settings.preset = 0u;
      settings.density = 1.0f;
      settings.radiusScale = 1.0f;
      settings.intensityScale = 1.0f;
      settings.directionalIntensity = 2.0f;
      break;
  }
  return settings;
}

void CheckVkResult(VkResult result) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error("ImGui Vulkan backend error");
  }
}

bool DrawTransformControls(const char* label,
                           TransformControls& transform,
                           float dragSpeed = 0.05f) {
  bool changed = false;
  if (ImGui::TreeNode(label)) {
    changed |= ImGui::DragFloat3("Position", &transform.position.x, dragSpeed);
    changed |=
        ImGui::DragFloat3("Rotation", &transform.rotationDegrees.x, 0.5f);
    changed |= ImGui::DragFloat3("Scale", &transform.scale.x, dragSpeed, 0.001f,
                                 1000.0f);
    ImGui::TreePop();
  }
  return changed;
}

void DrawRenderPassToggleEntry(RenderPassToggle& pass) {
  if (pass.locked) {
    ImGui::BeginDisabled();
    ImGui::Checkbox(pass.name.c_str(), &pass.enabled);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("core");
    return;
  }

  ImGui::Checkbox(pass.name.c_str(), &pass.enabled);
  if (pass.autoDisabled) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", pass.dependencyNote.empty()
                                  ? "disabled by dependency"
                                  : pass.dependencyNote.c_str());
  }
}

std::string_view RenderPassSectionName(std::string_view passName) {
  if (passName == "FrustumCull" || passName == "DepthPrepass" ||
      passName == "HiZGenerate" || passName == "OcclusionCull" ||
      passName == "CullStatsReadback" || passName == "GBuffer" ||
      passName == "DepthToReadOnly") {
    return "Culling";
  }

  if (passName == "ShadowCascade0" || passName == "ShadowCascade1" ||
      passName == "ShadowCascade2" || passName == "ShadowCascade3") {
    return "Shadows";
  }

  if (passName == "OitClear" || passName == "TileCull" ||
      passName == "GTAO" || passName == "Lighting" ||
      passName == "OitResolve") {
    return "Lighting";
  }

  return "Post-process";
}

}  // namespace

GuiManager::~GuiManager() = default;

void GuiManager::initialize(VkInstance instance, VkDevice device,
                            VkPhysicalDevice physicalDevice,
                            VkQueue graphicsQueue, uint32_t graphicsQueueFamily,
                            VkRenderPass renderPass, uint32_t imageCount,
                            GLFWwindow* window,
                            const std::string& defaultModelPath) {
  if (initialized_) return;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForVulkan(window, true);

  std::array<VkDescriptorPoolSize, 11> poolSizes = {{
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
      {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
  }};

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.maxSets = 1000 * static_cast<uint32_t>(poolSizes.size());
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();

  if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create ImGui descriptor pool");
  }

  ImGui_ImplVulkan_InitInfo initInfo{};
  initInfo.Instance = instance;
  initInfo.PhysicalDevice = physicalDevice;
  initInfo.Device = device;
  initInfo.QueueFamily = graphicsQueueFamily;
  initInfo.Queue = graphicsQueue;
  initInfo.DescriptorPool = descriptorPool_;
  initInfo.RenderPass = renderPass;
  initInfo.Subpass = 0;
  initInfo.MinImageCount = imageCount;
  initInfo.ImageCount = imageCount;
  initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  initInfo.CheckVkResultFn = CheckVkResult;

  ImGui_ImplVulkan_Init(&initInfo);
  ImGui_ImplVulkan_CreateFontsTexture();

  defaultModelPath_ = defaultModelPath;
  gltfPathInput_ = defaultModelPath;
  initialized_ = true;
}

void GuiManager::shutdown(VkDevice device) {
  if (!initialized_) return;

  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  if (descriptorPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
  }

  initialized_ = false;
}

void GuiManager::updateSwapchainImageCount(uint32_t imageCount) {
  if (!initialized_) return;
  ImGui_ImplVulkan_SetMinImageCount(imageCount);
}

void GuiManager::startFrame() {
  if (!initialized_) return;
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void GuiManager::render(VkCommandBuffer commandBuffer) {
  if (!initialized_) return;
  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void GuiManager::drawSceneControls(
    const container::scene::SceneGraph& sceneGraph,
    const std::function<bool(const std::string&)>& reloadModel,
    const std::function<bool()>& reloadDefault,
    const TransformControls& cameraTransform,
    const std::function<void(const TransformControls&)>& applyCameraTransform,
    const TransformControls& sceneTransform,
    const std::function<void(const TransformControls&)>& applySceneTransform,
    const glm::vec3& directionalLightPosition,
    const container::gpu::LightingData& lightingData,
    uint32_t selectedMeshNode,
    const std::function<void(uint32_t)>& selectMeshNode,
    const TransformControls& meshTransform,
    const std::function<void(uint32_t, const TransformControls&)>&
        applyMeshTransform) {
  if (!initialized_) return;

  static constexpr const char* kGBufferViewLabels[] = {
      "Lit",       "Albedo",      "Normals", "Material",
      "Depth",     "Emissive",    "Transparency",
      "Revealage", "Overview",    "Surface Normals",
      "Object Normals", "Shadow Cascades",
      "Tile Light Heat Map"};

  ImGui::Begin("Scene Controls");
  ImGui::InputText("glTF path", &gltfPathInput_);

  if (ImGui::Button("Load glTF")) {
    const bool success = reloadModel(gltfPathInput_);
    statusMessage_ = success ? "Loaded model: " + gltfPathInput_
                             : "Failed to load model: " + gltfPathInput_;
  }

  ImGui::SameLine();

  if (ImGui::Button("Reload Default")) {
    const bool success = reloadDefault();
    statusMessage_ = success ? "Loaded default test scene"
                             : "Failed to load default test scene";
    gltfPathInput_ = defaultModelPath_;
  }

  ImGui::Separator();
  int gBufferView = static_cast<int>(gBufferViewMode_);
  if (ImGui::Combo("Display", &gBufferView, kGBufferViewLabels,
                   IM_ARRAYSIZE(kGBufferViewLabels))) {
    gBufferViewMode_ = static_cast<GBufferViewMode>(gBufferView);
  }
  ImGui::Checkbox("Overlay vertices", &showGeometryOverlay_);
  ImGui::Checkbox("Overlay lights", &showLightGizmos_);
  ImGui::Checkbox("Normal validation", &normalValidationSettings_.enabled);
  if (normalValidationSettings_.enabled) {
    ImGui::Checkbox("Normal validation face fill",
                    &normalValidationSettings_.showFaceFill);
    ImGui::SliderFloat("Normal line length", &normalValidationSettings_.lineLength,
                       0.01f, 100.0f);
    ImGui::SliderFloat("Normal line offset", &normalValidationSettings_.lineOffset,
                       0.0f, 0.05f);
    ImGui::SliderFloat("Normal face alpha", &normalValidationSettings_.faceAlpha,
                       0.0f, 1.0f);
    if (wireframeWideLineSupported_) {
      ImGui::SliderFloat("Normal line width",
                         &normalValidationSettings_.lineWidth, 1.0f, 100.0f);
    } else {
      normalValidationSettings_.lineWidth = 1.0f;
      ImGui::BeginDisabled();
      ImGui::SliderFloat("Normal line width",
                         &normalValidationSettings_.lineWidth, 1.0f, 1.0f);
      ImGui::EndDisabled();
    }
  }

  ImGui::Separator();
  ImGui::Text("Wireframe Debug");
  ImGui::TextDisabled("Backend: %s",
                      wireframeRasterModeSupported_ ? "Native raster line"
                                                    : "Shader fallback");
  if (!wireframeSupported_) {
    ImGui::BeginDisabled();
  }
  ImGui::Checkbox("Wireframe Enabled", &wireframeSettings_.enabled);
  int wireframeMode = static_cast<int>(wireframeSettings_.mode);
  static constexpr const char* kWireframeModeLabels[] = {"Overlay", "Full"};
  if (ImGui::Combo("Wireframe Mode", &wireframeMode, kWireframeModeLabels,
                   IM_ARRAYSIZE(kWireframeModeLabels))) {
    wireframeSettings_.mode = static_cast<WireframeMode>(wireframeMode);
  }
  ImGui::Checkbox("Wireframe Depth Test", &wireframeSettings_.depthTest);
  ImGui::ColorEdit3("Wireframe Color", &wireframeSettings_.color.x);
  ImGui::SliderFloat("Wireframe Intensity", &wireframeSettings_.overlayIntensity,
                     0.0f, 1.0f);
  if (wireframeWideLineSupported_) {
    ImGui::SliderFloat("Wireframe Line Width", &wireframeSettings_.lineWidth,
                       1.0f, 8.0f);
  } else {
    wireframeSettings_.lineWidth = 1.0f;
    ImGui::BeginDisabled();
    ImGui::SliderFloat("Wireframe Line Width", &wireframeSettings_.lineWidth,
                       1.0f, 1.0f);
    ImGui::EndDisabled();
  }
  if (!wireframeSupported_) {
    wireframeSettings_.enabled = false;
    ImGui::EndDisabled();
    ImGui::TextDisabled("Wireframe unavailable: fillModeNonSolid unsupported");
  }
  ImGui::Text("Scene nodes: %zu", sceneGraph.nodeCount());
  ImGui::Text("Renderable primitives: %zu",
              sceneGraph.renderableNodes().size());

  if (cullStatsTotal_ > 0) {
    ImGui::Separator();
    ImGui::Text("GPU Culling");
    ImGui::Checkbox("Freeze culling camera (F8)", &freezeCulling_);
    if (freezeCulling_)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "CULLING FROZEN");
    ImGui::BulletText("Input: %u", cullStatsTotal_);
    ImGui::BulletText("Frustum passed: %u", cullStatsFrustum_);
    ImGui::BulletText("Occlusion passed: %u", cullStatsOcclusion_);
    ImGui::BulletText("Frustum culled: %u", cullStatsTotal_ - cullStatsFrustum_);
    if (cullStatsFrustum_ > 0)
      ImGui::BulletText("Occlusion culled: %u", cullStatsFrustum_ - cullStatsOcclusion_);
  }

  if (!renderPassToggles_.empty()) {
    ImGui::Separator();
    if (ImGui::TreeNode("Render Passes")) {
      if (ImGui::SmallButton("Enable All")) {
        for (auto& p : renderPassToggles_) p.enabled = true;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Disable All")) {
        // Keep PostProcess always on so the UI remains visible.
        for (auto& p : renderPassToggles_) {
          p.enabled = p.locked;
        }
      }

      auto drawRenderPassSection = [&](std::string_view sectionName) {
        if (!ImGui::TreeNode(std::string(sectionName).c_str())) {
          return;
        }

        for (auto& p : renderPassToggles_) {
          if (RenderPassSectionName(p.name) == sectionName) {
            DrawRenderPassToggleEntry(p);
          }
        }

        ImGui::TreePop();
      };

      drawRenderPassSection("Culling");
      drawRenderPassSection("Shadows");
      drawRenderPassSection("Lighting");
      drawRenderPassSection("Post-process");

      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Protected passes are shown as locked. Optional passes may be auto-disabled when a dependency is off.");
      }
      ImGui::TreePop();
    }
  }

  ImGui::Separator();
  ImGui::Text("Bloom");
  ImGui::Checkbox("Bloom Enabled", &bloomEnabled_);
  if (bloomEnabled_) {
    ImGui::SliderFloat("Bloom Threshold", &bloomThreshold_, 0.0f, 5.0f);
    ImGui::SliderFloat("Bloom Knee", &bloomKnee_, 0.0f, 1.0f);
    ImGui::SliderFloat("Bloom Intensity", &bloomIntensity_, 0.0f, 2.0f);
    ImGui::SliderFloat("Bloom Radius", &bloomRadius_, 0.1f, 3.0f);
  }

  TransformControls editableCameraTransform = cameraTransform;
  if (DrawTransformControls("Camera", editableCameraTransform)) {
    applyCameraTransform(editableCameraTransform);
  }

  TransformControls editableSceneTransform = sceneTransform;
  if (DrawTransformControls("Scene", editableSceneTransform)) {
    applySceneTransform(editableSceneTransform);
  }

  if (ImGui::TreeNode("Lights")) {
    ImGui::Text("Generator");
    int presetIndex = static_cast<int>(
        std::min<uint32_t>(lightingSettings_.preset,
                           static_cast<uint32_t>(kLightingPresetLabels.size() - 1u)));
    if (ImGui::Combo("Lighting Preset", &presetIndex,
                     kLightingPresetLabels.data(),
                     static_cast<int>(kLightingPresetLabels.size()))) {
      if (presetIndex < static_cast<int>(kLightingPresetLabels.size() - 1u)) {
        lightingSettings_ = LightingPreset(static_cast<uint32_t>(presetIndex));
      } else {
        lightingSettings_.preset =
            static_cast<uint32_t>(kLightingPresetLabels.size() - 1u);
      }
    }

    bool lightSliderChanged = false;
    lightSliderChanged |= ImGui::SliderFloat(
        "Light Density", &lightingSettings_.density, 0.1f, 16.0f, "%.2f");
    lightSliderChanged |= ImGui::SliderFloat(
        "Light Radius Scale", &lightingSettings_.radiusScale, 0.05f, 8.0f, "%.2f");
    lightSliderChanged |= ImGui::SliderFloat(
        "Light Intensity Scale", &lightingSettings_.intensityScale, 0.0f, 16.0f, "%.2f");
    lightSliderChanged |= ImGui::SliderFloat(
        "Directional Intensity", &lightingSettings_.directionalIntensity, 0.0f, 16.0f, "%.2f");
    if (lightSliderChanged) {
      lightingSettings_.preset =
          static_cast<uint32_t>(kLightingPresetLabels.size() - 1u);
    }

    ImGui::Separator();
    ImGui::Text("Cluster Stats");
    ImGui::BulletText("Submitted lights: %u", lightCullingStats_.submittedLights);
    ImGui::BulletText("Active clusters: %u / %u",
                      lightCullingStats_.activeClusters,
                      lightCullingStats_.totalClusters);
    ImGui::BulletText("Max lights per cluster: %u",
                      lightCullingStats_.maxLightsPerCluster);
    ImGui::BulletText("Dropped light refs: %u",
                      lightCullingStats_.droppedLightReferences);
    ImGui::BulletText("Cluster cull GPU: %.3f ms",
                      lightCullingStats_.clusterCullMs);
    ImGui::BulletText("Clustered lighting GPU: %.3f ms",
                      lightCullingStats_.clusteredLightingMs);

    ImGui::Separator();
    ImGui::Text("Directional");
    ImGui::BulletText("Position: (%.2f, %.2f, %.2f)", directionalLightPosition.x,
                      directionalLightPosition.y, directionalLightPosition.z);
    ImGui::BulletText("Direction: (%.2f, %.2f, %.2f)",
                      lightingData.directionalDirection.x,
                      lightingData.directionalDirection.y,
                      lightingData.directionalDirection.z);

    const uint32_t visiblePointLightCount =
        std::min(lightingData.pointLightCount, kMaxDeferredPointLights);
    ImGui::Text("Point lights: %u total", lightingData.pointLightCount);
    if (lightingData.pointLightCount > visiblePointLightCount) {
      ImGui::Text("Showing first %u UBO fallback lights", visiblePointLightCount);
    }
    for (uint32_t i = 0; i < visiblePointLightCount; ++i) {
      const auto& light = lightingData.pointLights[i];
      ImGui::Separator();
      ImGui::Text("Point Light %u", i);
      ImGui::BulletText("Position: (%.2f, %.2f, %.2f)",
                        light.positionRadius.x, light.positionRadius.y,
                        light.positionRadius.z);
      ImGui::BulletText("Radius: %.2f", light.positionRadius.w);
    }
    ImGui::TreePop();
  }

  const auto& renderableNodes = sceneGraph.renderableNodes();
  if (!renderableNodes.empty()) {
    uint32_t activeMeshNode = selectedMeshNode;
    if (!std::ranges::contains(renderableNodes, activeMeshNode)) {
      activeMeshNode = renderableNodes.front();
      selectMeshNode(activeMeshNode);
    }

    std::string selectedLabel = "Node " + std::to_string(activeMeshNode);
    if (const auto* node = sceneGraph.getNode(activeMeshNode);
        node != nullptr &&
        node->primitiveIndex != container::scene::SceneGraph::kInvalidNode) {
      selectedLabel += " / Primitive " + std::to_string(node->primitiveIndex);
    }

    if (ImGui::BeginCombo("Mesh", selectedLabel.c_str())) {
      for (uint32_t nodeIndex : renderableNodes) {
        std::string label = "Node " + std::to_string(nodeIndex);
        if (const auto* node = sceneGraph.getNode(nodeIndex);
            node != nullptr &&
            node->primitiveIndex != container::scene::SceneGraph::kInvalidNode) {
          label += " / Primitive " + std::to_string(node->primitiveIndex);
        }
        const bool selected = nodeIndex == activeMeshNode;
        if (ImGui::Selectable(label.c_str(), selected)) {
          activeMeshNode = nodeIndex;
          selectMeshNode(nodeIndex);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    TransformControls editableMeshTransform = meshTransform;
    if (DrawTransformControls("Mesh Transform", editableMeshTransform)) {
      applyMeshTransform(activeMeshNode, editableMeshTransform);
    }
  }

  if (!statusMessage_.empty()) {
    ImGui::TextWrapped("%s", statusMessage_.c_str());
  }

  if (!environmentStatus_.empty()) {
    ImGui::Separator();
    ImGui::Text("Environment");
    ImGui::TextWrapped("%s", environmentStatus_.c_str());
  }

  ImGui::End();
}

bool GuiManager::isCapturingInput() const {
  if (!initialized_) return false;
  const ImGuiIO& io = ImGui::GetIO();
  return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void GuiManager::setWireframeCapabilities(bool supported,
                                          bool rasterModeSupported,
                                          bool wideLineSupported) {
  wireframeSupported_ = supported;
  wireframeRasterModeSupported_ = supported && rasterModeSupported;
  wireframeWideLineSupported_ = wideLineSupported;
  if (!wireframeSupported_) {
    wireframeSettings_.enabled = false;
  }
  if (!wireframeWideLineSupported_) {
    wireframeSettings_.lineWidth = 1.0f;
  }
}

void GuiManager::setCullStats(uint32_t total, uint32_t frustumPassed,
                              uint32_t occlusionPassed) {
  cullStatsTotal_     = total;
  cullStatsFrustum_   = frustumPassed;
  cullStatsOcclusion_ = occlusionPassed;
}

void GuiManager::setLightCullingStats(
    const container::gpu::LightCullingStats& stats) {
  lightCullingStats_ = stats;
}

void GuiManager::setLightingSettings(
    const container::gpu::LightingSettings& settings) {
  lightingSettings_ = settings;
}

void GuiManager::setFreezeCulling(bool frozen) {
  freezeCulling_ = frozen;
}

void GuiManager::setBloomSettings(bool enabled, float threshold, float knee,
                                  float intensity, float radius) {
  bloomEnabled_   = enabled;
  bloomThreshold_ = threshold;
  bloomKnee_      = knee;
  bloomIntensity_ = intensity;
  bloomRadius_    = radius;
}

void GuiManager::setRenderPassList(const std::vector<RenderPassToggle>& passes) {
  // Rebuild the toggle list, preserving existing enabled states by name.
  std::vector<RenderPassToggle> updated;
  updated.reserve(passes.size());
  for (const auto& incoming : passes) {
    bool found = false;
    for (const auto& existing : renderPassToggles_) {
      if (existing.name == incoming.name) {
        RenderPassToggle merged = existing;
        merged.locked = incoming.locked;
        merged.autoDisabled = incoming.autoDisabled;
        merged.dependencyNote = incoming.dependencyNote;
        updated.push_back(std::move(merged));
        found = true;
        break;
      }
    }
    if (!found) {
      updated.push_back(incoming);
    }
  }
  renderPassToggles_ = std::move(updated);
}

void GuiManager::ensureInitialized() const {
  if (!initialized_) {
    throw std::runtime_error("GuiManager used before initialization");
  }
}

}  // namespace container::ui













