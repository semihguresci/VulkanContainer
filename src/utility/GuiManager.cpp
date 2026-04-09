#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

#include "Container/utility/GuiManager.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>


#include "Container/common/CommonMath.h"


namespace utility::ui {

namespace {

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
    const utility::scene::SceneGraph& sceneGraph,
    const std::function<bool(const std::string&)>& reloadModel,
    const std::function<bool()>& reloadDefault,
    const TransformControls& cameraTransform,
    const std::function<void(const TransformControls&)>& applyCameraTransform,
    const TransformControls& sceneTransform,
    const std::function<void(const TransformControls&)>& applySceneTransform,
    const glm::vec3& directionalLightPosition,
    const LightingData& lightingData,
    uint32_t selectedMeshNode,
    const std::function<void(uint32_t)>& selectMeshNode,
    const TransformControls& meshTransform,
    const std::function<void(uint32_t, const TransformControls&)>&
        applyMeshTransform) {
  if (!initialized_) return;

  static constexpr const char* kGBufferViewLabels[] = {
      "Lit",       "Albedo",      "Normals", "Material",
      "Depth",     "Emissive",    "Transparency",
      "Revealage", "Overview",    "Surface Normals"};

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
    statusMessage_ = success ? "Loaded default model: " + defaultModelPath_
                             : "Failed to load default model";
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
  ImGui::Checkbox("Normal diagnostic cube", &showNormalDiagCube_);
  ImGui::Text("Scene nodes: %zu", sceneGraph.nodeCount());
  ImGui::Text("Renderable primitives: %zu",
              sceneGraph.renderableNodes().size());

  TransformControls editableCameraTransform = cameraTransform;
  if (DrawTransformControls("Camera", editableCameraTransform)) {
    applyCameraTransform(editableCameraTransform);
  }

  TransformControls editableSceneTransform = sceneTransform;
  if (DrawTransformControls("Scene", editableSceneTransform)) {
    applySceneTransform(editableSceneTransform);
  }

  if (ImGui::TreeNode("Lights")) {
    ImGui::Text("Directional");
    ImGui::BulletText("Position: (%.2f, %.2f, %.2f)", directionalLightPosition.x,
                      directionalLightPosition.y, directionalLightPosition.z);
    ImGui::BulletText("Direction: (%.2f, %.2f, %.2f)",
                      lightingData.directionalDirection.x,
                      lightingData.directionalDirection.y,
                      lightingData.directionalDirection.z);

    const uint32_t pointLightCount =
        std::min(lightingData.pointLightCount, kMaxDeferredPointLights);
    for (uint32_t i = 0; i < pointLightCount; ++i) {
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
        node->primitiveIndex != utility::scene::SceneGraph::kInvalidNode) {
      selectedLabel += " / Primitive " + std::to_string(node->primitiveIndex);
    }

    if (ImGui::BeginCombo("Mesh", selectedLabel.c_str())) {
      for (uint32_t nodeIndex : renderableNodes) {
        std::string label = "Node " + std::to_string(nodeIndex);
        if (const auto* node = sceneGraph.getNode(nodeIndex);
            node != nullptr &&
            node->primitiveIndex != utility::scene::SceneGraph::kInvalidNode) {
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

  ImGui::End();
}

bool GuiManager::isCapturingInput() const {
  if (!initialized_) return false;
  const ImGuiIO& io = ImGui::GetIO();
  return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void GuiManager::ensureInitialized() const {
  if (!initialized_) {
    throw std::runtime_error("GuiManager used before initialization");
  }
}

}  // namespace utility::ui
