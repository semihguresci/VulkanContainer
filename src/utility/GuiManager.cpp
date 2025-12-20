#include <Container/utility/GuiManager.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <stdexcept>

namespace utility::ui {
namespace {
void CheckVkResult(VkResult result) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error("ImGui Vulkan backend error: " + std::to_string(result));
  }
}
}  // namespace

GuiManager::~GuiManager() = default;

void GuiManager::initialize(VkInstance instance, VkDevice device,
                            VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
                            uint32_t graphicsQueueFamily, VkRenderPass renderPass,
                            uint32_t imageCount, GLFWwindow* window,
                            const std::string& defaultModelPath) {
  if (initialized_) return;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForVulkan(window, true);

  std::array<VkDescriptorPoolSize, 11> poolSizes = {
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.maxSets = 1000 * static_cast<uint32_t>(poolSizes.size());
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();

  CheckVkResult(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_));

  ImGui_ImplVulkan_InitInfo initInfo{};
  initInfo.Instance = instance;
  initInfo.PhysicalDevice = physicalDevice;
  initInfo.Device = device;
  initInfo.QueueFamily = graphicsQueueFamily;
  initInfo.Queue = graphicsQueue;
  initInfo.DescriptorPool = descriptorPool_;
  initInfo.Subpass = 0;
  initInfo.MinImageCount = imageCount;
  initInfo.ImageCount = imageCount;
  initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  initInfo.CheckVkResultFn = &CheckVkResult;

  ImGui_ImplVulkan_Init(&initInfo, renderPass);

  // Upload fonts
  {
    VkCommandPoolCreateInfo pool_create_info{};
    pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_create_info.queueFamilyIndex = graphicsQueueFamily;

    VkCommandPool uploadCommandPool{};
    CheckVkResult(vkCreateCommandPool(device, &pool_create_info, nullptr, &uploadCommandPool));

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = uploadCommandPool;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer commandBuffer{};
    CheckVkResult(vkAllocateCommandBuffers(device, &alloc_info, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVkResult(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
    CheckVkResult(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    CheckVkResult(vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
    CheckVkResult(vkQueueWaitIdle(graphicsQueue));

    ImGui_ImplVulkan_DestroyFontUploadObjects();
    vkDestroyCommandPool(device, uploadCommandPool, nullptr);
  }

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
    const utility::scene::SceneGraph& sceneGraph, uint32_t maxSceneObjects,
    const std::function<void(const glm::mat4&)>& addObject,
    const std::function<void()>& addAutoOffsetObject,
    const std::function<bool(const std::string&)>& reloadModel,
    const std::function<bool()>& reloadDefault) {
  if (!initialized_) return;

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
  ImGui::InputFloat3("New object offset", &newObjectTranslation_.x);
  const bool canAddObject =
      sceneGraph.renderableNodes().size() < static_cast<size_t>(maxSceneObjects);
  if (!canAddObject) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Add Object")) {
    addObject(glm::translate(glm::mat4(1.0f), newObjectTranslation_));
  }
  ImGui::SameLine();
  if (ImGui::Button("Add Offset Object")) {
    addAutoOffsetObject();
  }
  if (!canAddObject) {
    ImGui::EndDisabled();
  }

  ImGui::Separator();
  ImGui::Text("Renderable objects: %zu / %u", sceneGraph.renderableNodes().size(),
              maxSceneObjects);
  if (!statusMessage_.empty()) {
    ImGui::TextWrapped("%s", statusMessage_.c_str());
  }
  ImGui::End();
}

bool GuiManager::isCapturingInput() const {
  if (!initialized_) return false;
  const auto& io = ImGui::GetIO();
  return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void GuiManager::ensureInitialized() const {
  if (!initialized_) {
    throw std::runtime_error("GuiManager used before initialization");
  }
}

}  // namespace utility::ui
