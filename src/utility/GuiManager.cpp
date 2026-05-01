#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "Container/utility/GuiManager.h"

#include "Container/common/CommonGLFW.h"
#include "Container/utility/Platform.h"
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

constexpr std::string_view kSampleModelsRelativeRoot =
    "models/glTF-Sample-Models/2.0";

constexpr std::array<const char*, 4> kLightingPresetLabels = {{
    "Sponza", "Interior", "Exterior", "Custom",
}};

constexpr std::array<const char*, 3> kImportScaleLabels = {{
    "1x", "10x", "100x",
}};

constexpr std::array<float, 3> kImportScaleValues = {{
    1.0f, 10.0f, 100.0f,
}};

std::string ToLowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool IsGltfModelFile(const std::filesystem::path& path) {
  const std::string extension =
      ToLowerAscii(container::util::pathToUtf8(path.extension()));
  return extension == ".gltf" || extension == ".glb";
}

std::filesystem::path ComparableModelPath(const std::filesystem::path& input) {
  std::filesystem::path path = input;
  if (path.is_relative()) {
    std::error_code existsError;
    const auto exeRelative =
        container::util::executableDirectory() / std::filesystem::path(path);
    if (std::filesystem::exists(exeRelative, existsError)) {
      path = exeRelative;
    } else {
      std::error_code currentPathError;
      const auto workingDirectory = std::filesystem::current_path(currentPathError);
      if (!currentPathError) {
        path = workingDirectory / path;
      }
    }
  }

  std::error_code canonicalError;
  const auto canonical = std::filesystem::weakly_canonical(path, canonicalError);
  if (!canonicalError) {
    return canonical.lexically_normal();
  }

  std::error_code absoluteError;
  const auto absolute = std::filesystem::absolute(path, absoluteError);
  if (!absoluteError) {
    return absolute.lexically_normal();
  }

  return path.lexically_normal();
}

std::string ModelPathKey(const std::filesystem::path& path) {
  std::string key = container::util::pathToUtf8(ComparableModelPath(path));
#ifdef _WIN32
  key = ToLowerAscii(std::move(key));
#endif
  return key;
}

std::optional<std::filesystem::path> ResolveSampleModelsRoot() {
  const std::array<std::filesystem::path, 2> candidates = {
      container::util::executableDirectory() /
          std::filesystem::path(kSampleModelsRelativeRoot),
      std::filesystem::current_path() /
          std::filesystem::path(kSampleModelsRelativeRoot),
  };

  for (const auto& candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_directory(candidate, error)) {
      return candidate;
    }
  }

  return std::nullopt;
}

std::optional<std::filesystem::path> FindFirstModelFileInDirectory(
    const std::filesystem::path& directory) {
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return std::nullopt;
  }

  std::vector<std::filesystem::path> candidates;
  for (std::filesystem::directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code fileError;
    if (it->is_regular_file(fileError) && IsGltfModelFile(it->path())) {
      candidates.push_back(it->path());
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  std::ranges::sort(candidates);
  const auto gltf = std::ranges::find_if(candidates, [](const auto& path) {
    return ToLowerAscii(container::util::pathToUtf8(path.extension())) ==
           ".gltf";
  });
  return gltf != candidates.end() ? *gltf : candidates.front();
}

std::optional<std::filesystem::path> FindFirstModelFileRecursive(
    const std::filesystem::path& directory) {
  std::error_code error;
  std::vector<std::filesystem::path> candidates;
  for (std::filesystem::recursive_directory_iterator it(
           directory, std::filesystem::directory_options::skip_permission_denied,
           error),
       end;
       !error && it != end; it.increment(error)) {
    std::error_code fileError;
    if (it->is_regular_file(fileError) && IsGltfModelFile(it->path())) {
      candidates.push_back(it->path());
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  std::ranges::sort(candidates);
  return candidates.front();
}

std::optional<std::filesystem::path> PreferredSampleModelPath(
    const std::filesystem::path& modelDirectory,
    std::string& selectedVariant) {
  static constexpr std::array<std::string_view, 6> kPreferredVariantFolders = {{
      "glTF",
      "glTF-Binary",
      "glTF-Embedded",
      "glTF-MaterialsCommon",
      "glTF-pbrSpecularGlossiness",
      "glTF-KTX-BasisU",
  }};

  for (std::string_view variant : kPreferredVariantFolders) {
    const auto variantDirectory =
        modelDirectory / std::filesystem::path(variant);
    if (auto path = FindFirstModelFileInDirectory(variantDirectory)) {
      selectedVariant = std::string(variant);
      return path;
    }
  }

  if (auto path = FindFirstModelFileRecursive(modelDirectory)) {
    selectedVariant =
        container::util::pathToUtf8(path->parent_path().filename());
    return path;
  }

  return std::nullopt;
}

std::string ImportScaleLabel(float scale) {
  for (size_t i = 0; i < kImportScaleValues.size(); ++i) {
    if (scale == kImportScaleValues[i]) {
      return kImportScaleLabels[i];
    }
  }
  return std::to_string(scale) + "x";
}

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

constexpr size_t TelemetryPhaseIndex(
    container::renderer::RendererTelemetryPhase phase) {
  return static_cast<size_t>(phase);
}

float TelemetryPhaseMs(
    const container::renderer::RendererTelemetrySnapshot& snapshot,
    container::renderer::RendererTelemetryPhase phase) {
  if (phase == container::renderer::RendererTelemetryPhase::Count) {
    return 0.0f;
  }
  return snapshot.timing.cpuMs[TelemetryPhaseIndex(phase)];
}

float PercentOf(float value, float total) {
  return total > 0.0f ? (value / total) * 100.0f : 0.0f;
}

void ImGuiTextStringView(const char* label, std::string_view value) {
  ImGui::Text("%s: %.*s", label, static_cast<int>(value.size()),
              value.data());
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
                            const std::string& defaultModelPath,
                            float defaultImportScale) {
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
  const auto importScaleIt =
      std::ranges::find(kImportScaleValues, defaultImportScale);
  importScaleIndex_ =
      importScaleIt != kImportScaleValues.end()
          ? static_cast<int>(importScaleIt - kImportScaleValues.begin())
          : 0;
  importScale_ = kImportScaleValues[static_cast<size_t>(importScaleIndex_)];
  discoverSampleModels();
  initialized_ = true;
}

void GuiManager::discoverSampleModels() {
  sampleModelOptions_.clear();
  selectedSampleModelIndex_ = -1;

  const auto sampleRoot = ResolveSampleModelsRoot();
  if (!sampleRoot) {
    return;
  }

  std::error_code error;
  for (std::filesystem::directory_iterator it(*sampleRoot, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code directoryError;
    if (!it->is_directory(directoryError)) {
      continue;
    }

    std::string selectedVariant;
    const auto modelPath = PreferredSampleModelPath(it->path(), selectedVariant);
    if (!modelPath) {
      continue;
    }

    SampleModelOption option{};
    option.label = container::util::pathToUtf8(it->path().filename());
    if (!selectedVariant.empty() && selectedVariant != "glTF") {
      option.label += " (" + selectedVariant + ")";
    }
    option.path = container::util::pathToUtf8(*modelPath);
    sampleModelOptions_.push_back(std::move(option));
  }

  std::ranges::sort(sampleModelOptions_, {}, &SampleModelOption::label);
  selectedSampleModelIndex_ = sampleModelIndexForPath(gltfPathInput_);
}

int GuiManager::sampleModelIndexForPath(const std::string& path) const {
  if (path.empty()) {
    return -1;
  }

  const std::string target =
      ModelPathKey(container::util::pathFromUtf8(path));
  for (size_t i = 0; i < sampleModelOptions_.size(); ++i) {
    if (ModelPathKey(container::util::pathFromUtf8(
            sampleModelOptions_[i].path)) == target) {
      return static_cast<int>(i);
    }
  }

  return -1;
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
    const std::function<bool(const std::string&, float)>& reloadModel,
    const std::function<bool(float)>& reloadDefault,
    const TransformControls& cameraTransform,
    const std::function<void(const TransformControls&)>& applyCameraTransform,
    const TransformControls& sceneTransform,
    const std::function<void(const TransformControls&)>& applySceneTransform,
    const glm::vec3& directionalLightPosition,
    const container::gpu::LightingData& lightingData,
    const std::vector<container::gpu::PointLightData>& pointLights,
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
      "Tile Light Heat Map", "Shadow Texel Density"};

  ImGui::Begin("Scene Controls");
  if (ImGui::Combo("Import scale", &importScaleIndex_,
                   kImportScaleLabels.data(),
                   static_cast<int>(kImportScaleLabels.size()))) {
    importScaleIndex_ =
        std::clamp(importScaleIndex_, 0,
                   static_cast<int>(kImportScaleValues.size()) - 1);
    importScale_ = kImportScaleValues[static_cast<size_t>(importScaleIndex_)];
    statusMessage_ = "Import scale set to " + ImportScaleLabel(importScale_) +
                     "; reload model to apply";
  }

  if (!sampleModelOptions_.empty()) {
    const bool hasSelection =
        selectedSampleModelIndex_ >= 0 &&
        selectedSampleModelIndex_ <
            static_cast<int>(sampleModelOptions_.size());
    const char* preview =
        hasSelection ? sampleModelOptions_[selectedSampleModelIndex_].label.c_str()
                     : "Select sample model";
    if (ImGui::BeginCombo("Sample model", preview)) {
      for (int i = 0; i < static_cast<int>(sampleModelOptions_.size()); ++i) {
        const auto& option = sampleModelOptions_[i];
        const bool selected = i == selectedSampleModelIndex_;
        if (ImGui::Selectable(option.label.c_str(), selected)) {
          selectedSampleModelIndex_ = i;
          gltfPathInput_ = option.path;
          const bool success = reloadModel(gltfPathInput_, importScale_);
          statusMessage_ =
              success ? "Loaded model: " + option.label + " @ " +
                            ImportScaleLabel(importScale_)
                      : "Failed to load model: " + option.label;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", option.path.c_str());
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  } else {
    ImGui::TextDisabled("No sample models found");
  }

  ImGui::InputText("glTF path", &gltfPathInput_);

  if (ImGui::Button("Load glTF")) {
    const bool success = reloadModel(gltfPathInput_, importScale_);
    selectedSampleModelIndex_ = sampleModelIndexForPath(gltfPathInput_);
    statusMessage_ = success ? "Loaded model: " + gltfPathInput_ + " @ " +
                                   ImportScaleLabel(importScale_)
                             : "Failed to load model: " + gltfPathInput_;
  }

  ImGui::SameLine();

  if (ImGui::Button("Reload Default")) {
    const bool success = reloadDefault(importScale_);
    statusMessage_ = success ? "Loaded default model @ " +
                                   ImportScaleLabel(importScale_)
                             : "Failed to load default model";
    gltfPathInput_ = defaultModelPath_;
    selectedSampleModelIndex_ = sampleModelIndexForPath(gltfPathInput_);
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
  ImGui::Text("Post-process");
  int exposureMode =
      exposureSettings_.mode == container::gpu::kExposureModeAuto ? 1 : 0;
  static constexpr const char* kExposureModeLabels[] = {"Manual", "Auto"};
  if (ImGui::Combo("Exposure Mode", &exposureMode, kExposureModeLabels,
                   IM_ARRAYSIZE(kExposureModeLabels))) {
    exposureSettings_.mode = exposureMode == 1
                                 ? container::gpu::kExposureModeAuto
                                 : container::gpu::kExposureModeManual;
  }
  ImGui::SliderFloat("Exposure", &exposureSettings_.manualExposure, 0.0f,
                     4.0f, "%.3f");
  if (exposureSettings_.mode == container::gpu::kExposureModeAuto) {
    ImGui::SliderFloat("Target Luminance",
                       &exposureSettings_.targetLuminance, 0.01f, 2.0f,
                       "%.3f");
    ImGui::SliderFloat("Min Exposure", &exposureSettings_.minExposure,
                       0.0f, 4.0f, "%.3f");
    ImGui::SliderFloat("Max Exposure", &exposureSettings_.maxExposure,
                       0.0f, 16.0f, "%.3f");
    exposureSettings_.maxExposure =
        std::max(exposureSettings_.maxExposure,
                 exposureSettings_.minExposure);
    ImGui::SliderFloat("Adaptation Rate",
                       &exposureSettings_.adaptationRate, 0.0f, 10.0f,
                       "%.2f");
    ImGui::SliderFloat("Low Percentile",
                       &exposureSettings_.meteringLowPercentile, 0.0f, 0.99f,
                       "%.2f");
    ImGui::SliderFloat("High Percentile",
                       &exposureSettings_.meteringHighPercentile,
                       exposureSettings_.meteringLowPercentile + 0.01f, 1.0f,
                       "%.2f");
    exposureSettings_.meteringHighPercentile =
        std::max(exposureSettings_.meteringHighPercentile,
                 exposureSettings_.meteringLowPercentile + 0.01f);
  }
  ImGui::Text("Bloom");
  ImGui::Checkbox("Bloom Enabled", &bloomEnabled_);
  if (bloomEnabled_) {
    ImGui::SliderFloat("Bloom Threshold", &bloomThreshold_, 0.0f, 5.0f);
    ImGui::SliderFloat("Bloom Knee", &bloomKnee_, 0.0f, 1.0f);
    ImGui::SliderFloat("Bloom Intensity", &bloomIntensity_, 0.0f, 2.0f);
    ImGui::SliderFloat("Bloom Radius", &bloomRadius_, 0.1f, 3.0f);
  }

  if (ImGui::TreeNode("Shadows")) {
    ImGui::SliderFloat("Normal Bias Min Texels",
                       &shadowSettings_.normalBiasMinTexels, 0.0f, 8.0f,
                       "%.2f");
    ImGui::SliderFloat("Normal Bias Max Texels",
                       &shadowSettings_.normalBiasMaxTexels, 0.0f, 12.0f,
                       "%.2f");
    shadowSettings_.normalBiasMaxTexels =
        std::max(shadowSettings_.normalBiasMaxTexels,
                 shadowSettings_.normalBiasMinTexels);
    ImGui::SliderFloat("Slope Bias Scale", &shadowSettings_.slopeBiasScale,
                       0.0f, 0.01f, "%.5f");
    ImGui::SliderFloat("Receiver Bias Scale",
                       &shadowSettings_.receiverPlaneBiasScale, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SliderFloat("Filter Radius Texels",
                       &shadowSettings_.filterRadiusTexels, 0.25f, 3.0f,
                       "%.2f");
    ImGui::SliderFloat("Cascade Blend", &shadowSettings_.cascadeBlendFraction,
                       0.0f, 0.45f, "%.2f");
    ImGui::SliderFloat("Constant Depth Bias",
                       &shadowSettings_.constantDepthBias, 0.0f, 0.005f,
                       "%.5f");
    ImGui::SliderFloat("Max Depth Bias", &shadowSettings_.maxDepthBias,
                       0.0f, 0.02f, "%.5f");
    ImGui::SliderFloat("Raster Constant Bias",
                       &shadowSettings_.rasterConstantBias, -16.0f, 0.0f,
                       "%.2f");
    ImGui::SliderFloat("Raster Slope Bias",
                       &shadowSettings_.rasterSlopeBias, -8.0f, 0.0f,
                       "%.2f");
    ImGui::TreePop();
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
        "Point Intensity Scale", &lightingSettings_.intensityScale, 0.0f,
        16.0f, "%.2f");
    lightSliderChanged |= ImGui::SliderFloat(
        "Directional Intensity", &lightingSettings_.directionalIntensity, 0.0f, 16.0f, "%.2f");
    lightSliderChanged |= ImGui::SliderFloat(
        "Environment Intensity", &lightingSettings_.environmentIntensity,
        0.0f, 16.0f, "%.2f");
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

    const uint32_t submittedPointLightCount =
        static_cast<uint32_t>(std::min<size_t>(pointLights.size(), UINT32_MAX));
    const uint32_t visiblePointLightCount =
        std::min(submittedPointLightCount, kMaxDeferredPointLights);
    ImGui::Text("Point lights: %u submitted, %u uploaded",
                submittedPointLightCount, lightingData.pointLightCount);
    if (submittedPointLightCount > visiblePointLightCount) {
      ImGui::Text("Showing first %u SSBO lights", visiblePointLightCount);
    }
    for (uint32_t i = 0; i < visiblePointLightCount; ++i) {
      const auto& light = pointLights[i];
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

  drawRendererTelemetryWindow();
}

void GuiManager::drawRendererTelemetryWindow() {
  if (!initialized_) return;

  const auto& telemetry = rendererTelemetry_;
  if (!ImGui::Begin("Renderer Telemetry")) {
    ImGui::End();
    return;
  }

  const auto& latest = telemetry.latest;
  if (!latest.valid) {
    ImGui::TextDisabled("No renderer telemetry captured yet");
    ImGui::End();
    return;
  }

  const float frameMs = TelemetryPhaseMs(
      latest, container::renderer::RendererTelemetryPhase::Frame);
  const float fps = frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;
  const float gpuKnownMs = latest.timing.gpuKnownMs;

  if (ImGui::BeginTable("TelemetrySummary", 4,
                        ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("FPS");
    ImGui::Text("%.1f", fps);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("CPU frame");
    ImGui::Text("%.3f ms", frameMs);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("GPU known");
    ImGui::Text("%.3f ms", gpuKnownMs);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("CPU p95");
    ImGui::Text("%.3f ms", telemetry.summary.p95CpuFrameMs);
    ImGui::EndTable();
  }

  ImGui::Text("Frame: %llu  Image: %u  Slot: %u / %u",
              static_cast<unsigned long long>(latest.frameIndex),
              latest.imageIndex, latest.sync.frameSlot,
              latest.sync.maxFramesInFlight);
  ImGuiTextStringView(
      "GPU timing source",
      container::renderer::rendererGpuTimingSourceName(
          latest.timing.gpuSource));
  ImGui::Text("GPU profiler: %s",
              latest.gpuProfiler.available ? "available" : "unavailable");
  if (latest.gpuProfiler.resultLatencyFrames > 0u) {
    ImGui::Text("GPU result latency: %u frame",
                latest.gpuProfiler.resultLatencyFrames);
  }
  if (!latest.gpuProfiler.status.empty()) {
    ImGui::TextWrapped("%s", latest.gpuProfiler.status.c_str());
  }
  ImGui::Text("Swapchain: %ux%u, %u images",
              latest.resources.swapchainWidth,
              latest.resources.swapchainHeight,
              latest.resources.swapchainImageCount);
  if (latest.sync.serializedConcurrency) {
    ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.22f, 1.0f),
                       "Concurrency: serialized");
    if (!latest.sync.concurrencyReason.empty()) {
      ImGui::TextWrapped("%s", latest.sync.concurrencyReason.c_str());
    }
  } else {
    ImGui::Text("Concurrency: current frame fence");
  }

  if (!telemetry.history.empty()) {
    std::vector<float> cpuHistory;
    std::vector<float> gpuHistory;
    cpuHistory.reserve(telemetry.history.size());
    gpuHistory.reserve(telemetry.history.size());
    for (const auto& sample : telemetry.history) {
      cpuHistory.push_back(sample.cpuFrameMs);
      gpuHistory.push_back(sample.gpuKnownMs);
    }

    const float cpuScaleMax =
        std::max({16.67f, telemetry.summary.maxCpuFrameMs, frameMs});
    ImGui::PlotLines("CPU frame ms", cpuHistory.data(),
                     static_cast<int>(cpuHistory.size()), 0, nullptr, 0.0f,
                     cpuScaleMax, ImVec2(0.0f, 72.0f));
    const float gpuScaleMax =
        std::max({1.0f, telemetry.summary.maxGpuKnownMs, gpuKnownMs});
    ImGui::PlotLines("Known GPU ms", gpuHistory.data(),
                     static_cast<int>(gpuHistory.size()), 0, nullptr, 0.0f,
                     gpuScaleMax, ImVec2(0.0f, 72.0f));
  }

  if (ImGui::CollapsingHeader("CPU phases", ImGuiTreeNodeFlags_DefaultOpen)) {
    static constexpr std::array<container::renderer::RendererTelemetryPhase, 12>
        kPhases{{
            container::renderer::RendererTelemetryPhase::WaitForFrame,
            container::renderer::RendererTelemetryPhase::Readbacks,
            container::renderer::RendererTelemetryPhase::AcquireImage,
            container::renderer::RendererTelemetryPhase::ImageFenceWait,
            container::renderer::RendererTelemetryPhase::ResourceGrowth,
            container::renderer::RendererTelemetryPhase::Gui,
            container::renderer::RendererTelemetryPhase::SceneUpdate,
            container::renderer::RendererTelemetryPhase::DescriptorUpdate,
            container::renderer::RendererTelemetryPhase::CommandRecord,
            container::renderer::RendererTelemetryPhase::QueueSubmit,
            container::renderer::RendererTelemetryPhase::Screenshot,
            container::renderer::RendererTelemetryPhase::Present,
        }};
    static constexpr std::array<const char*, kPhases.size()> kLabels{{
        "Frame wait",
        "Readbacks",
        "Acquire image",
        "Image fence",
        "Resource growth",
        "GUI",
        "Scene update",
        "Descriptors",
        "Command record",
        "Queue submit",
        "Screenshot",
        "Present",
    }};

    if (ImGui::BeginTable("TelemetryCpuPhases", 3,
                          ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Phase");
      ImGui::TableSetupColumn("CPU ms");
      ImGui::TableSetupColumn("Frame %");
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < kPhases.size(); ++i) {
        const float phaseMs = TelemetryPhaseMs(latest, kPhases[i]);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kLabels[i]);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", phaseMs);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f%%", PercentOf(phaseMs, frameMs));
      }
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Workload and culling",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Objects: %u / capacity %u", latest.workload.objectCount,
                latest.resources.objectBufferCapacity);
    ImGui::Text("Draws: %u total, %u opaque, %u transparent",
                latest.workload.totalDrawCount,
                latest.workload.opaqueDrawCount,
                latest.workload.transparentDrawCount);
    ImGui::Text("OIT nodes: %u", latest.resources.oitNodeCapacity);

    const uint32_t frustumCulled =
        latest.culling.inputCount >= latest.culling.frustumPassedCount
            ? latest.culling.inputCount - latest.culling.frustumPassedCount
            : 0u;
    const uint32_t occlusionCulled =
        latest.culling.frustumPassedCount >=
                latest.culling.occlusionPassedCount
            ? latest.culling.frustumPassedCount -
                  latest.culling.occlusionPassedCount
            : 0u;
    ImGui::Separator();
    ImGui::Text("Culling");
    ImGui::Text("Input: %u", latest.culling.inputCount);
    ImGui::Text("Frustum passed: %u, culled: %u",
                latest.culling.frustumPassedCount, frustumCulled);
    ImGui::Text("Occlusion passed: %u, culled: %u",
                latest.culling.occlusionPassedCount, occlusionCulled);

    ImGui::Separator();
    ImGui::Text("Lighting");
    ImGui::Text("Submitted lights: %u", latest.lightCulling.submittedLights);
    ImGui::Text("Active clusters: %u / %u",
                latest.lightCulling.activeClusters,
                latest.lightCulling.totalClusters);
    ImGui::Text("Max lights per cluster: %u",
                latest.lightCulling.maxLightsPerCluster);
    ImGui::Text("Dropped light refs: %u",
                latest.lightCulling.droppedLightReferences);
    ImGui::Text("Cluster cull GPU: %.3f ms",
                latest.lightCulling.clusterCullMs);
    ImGui::Text("Clustered lighting GPU: %.3f ms",
                latest.lightCulling.clusteredLightingMs);
  }

  if (ImGui::CollapsingHeader("Render graph",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Passes: %u total, %u enabled, %u active, %u skipped",
                latest.graph.totalPasses, latest.graph.enabledPasses,
                latest.graph.activePasses, latest.graph.skippedPasses);
    ImGui::Text("Timed passes: %u CPU, %u GPU",
                latest.graph.cpuTimedPasses, latest.graph.gpuTimedPasses);
    if (ImGui::BeginTable("TelemetryRenderGraph", 6,
                          ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 220.0f))) {
      ImGui::TableSetupColumn("Pass");
      ImGui::TableSetupColumn("Enabled");
      ImGui::TableSetupColumn("State");
      ImGui::TableSetupColumn("CPU ms");
      ImGui::TableSetupColumn("Known GPU ms");
      ImGui::TableSetupColumn("Blocker");
      ImGui::TableHeadersRow();
      for (const auto& pass : latest.passes) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.enabled ? "yes" : "no");
        ImGui::TableNextColumn();
        if (pass.active) {
          ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.38f, 1.0f), "Active");
        } else {
          ImGui::TextColored(ImVec4(0.9f, 0.55f, 0.18f, 1.0f), "%s",
                             pass.status.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", pass.cpuRecordMs);
        ImGui::TableNextColumn();
        if (pass.gpuTimed) {
          ImGui::Text("%.3f", pass.gpuKnownMs);
        } else {
          ImGui::TextDisabled("-");
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.blocker.c_str());
      }
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Synchronization")) {
    ImGui::Text("Wait for frame: %.3f ms",
                TelemetryPhaseMs(
                    latest,
                    container::renderer::RendererTelemetryPhase::WaitForFrame));
    ImGui::Text("Image fence wait: %.3f ms",
                TelemetryPhaseMs(
                    latest,
                    container::renderer::RendererTelemetryPhase::ImageFenceWait));
    ImGui::Text("Acquire image: %.3f ms",
                TelemetryPhaseMs(
                    latest,
                    container::renderer::RendererTelemetryPhase::AcquireImage));
    ImGui::Text("Present: %.3f ms",
                TelemetryPhaseMs(
                    latest,
                    container::renderer::RendererTelemetryPhase::Present));
    ImGui::Text("Swapchain recreates: %llu",
                static_cast<unsigned long long>(
                    latest.sync.swapchainRecreateCount));
    ImGui::Text("Device wait idle calls: %llu",
                static_cast<unsigned long long>(
                    latest.sync.deviceWaitIdleCount));
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

void GuiManager::setRendererTelemetry(
    const container::renderer::RendererTelemetryView& telemetry) {
  rendererTelemetry_ = telemetry;
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













