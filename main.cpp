
#include "Container/common/CommonGLFW.h"
#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"

#include "Container/app/AppConfig.h"
#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/Model.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanAlignment.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanInstance.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/WindowManager.h"
#include "Container/utility/DebugMessengerExt.h"

#include <algorithm>
#include <array>
#include <span>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>

#include <limits>
#include <memory>
#include <print>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>


using utility::FrameSyncManager;
using utility::QueueFamilyIndices;
using utility::SwapChainManager;
using utility::SwapChainSupportDetails;
using utility::debug::CreateDebugUtilsMessengerEXT;
using utility::debug::debugCallback;
using utility::debug::DestroyDebugUtilsMessengerEXT;
namespace window = utility::window;

namespace {

struct AttachmentImage {
  VkImage image{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  VkImageView view{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
};

struct FrameResources {
  AttachmentImage albedo{};
  AttachmentImage normal{};
  AttachmentImage material{};
  AttachmentImage emissive{};
  AttachmentImage position{};
  AttachmentImage depthStencil{};
  AttachmentImage sceneColor{};
  AttachmentImage oitHeadPointers{};
  utility::memory::AllocatedBuffer oitNodeBuffer{};
  utility::memory::AllocatedBuffer oitCounterBuffer{};
  utility::memory::AllocatedBuffer oitMetadataBuffer{};
  uint32_t oitNodeCapacity{0};
  VkFramebuffer depthPrepassFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer gBufferFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer lightingFramebuffer{VK_NULL_HANDLE};
  VkDescriptorSet lightingDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet postProcessDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

struct DrawCommand {
  uint32_t objectIndex{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
};

struct LightPushConstants {
  glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
};

struct SceneLightingAnchor {
  glm::mat4 sceneTransform{1.0f};
  glm::vec3 center{0.0f, 0.0f, 0.0f};
  float localRadius{1.0f};
  float worldRadius{1.0f};
};

struct PostProcessPushConstants {
  uint32_t outputMode{0};
};

inline constexpr uint32_t kInvalidOitNodeIndex =
    std::numeric_limits<uint32_t>::max();

struct ExactOitNode {
  alignas(16) glm::vec4 color{0.0f};
  alignas(4) float depth{0.0f};
  alignas(4) uint32_t next{kInvalidOitNodeIndex};
  alignas(8) glm::vec2 padding{0.0f};
};

struct ExactOitMetadata {
  alignas(4) uint32_t nodeCapacity{0};
  alignas(4) uint32_t viewportWidth{0};
  alignas(4) uint32_t viewportHeight{0};
  alignas(4) uint32_t reserved{0};
};

utility::ui::TransformControls decomposeTransform(const glm::mat4& matrix) {
  utility::ui::TransformControls controls{};
  controls.position = glm::vec3(matrix[3]);

  glm::vec3 basisX = glm::vec3(matrix[0]);
  glm::vec3 basisY = glm::vec3(matrix[1]);
  glm::vec3 basisZ = glm::vec3(matrix[2]);
  controls.scale = glm::vec3(glm::length(basisX), glm::length(basisY),
                             glm::length(basisZ));
  controls.scale = glm::max(controls.scale, glm::vec3(0.001f));

  glm::mat3 rotationMatrix{};
  rotationMatrix[0] = basisX / controls.scale.x;
  rotationMatrix[1] = basisY / controls.scale.y;
  rotationMatrix[2] = basisZ / controls.scale.z;
  if (glm::determinant(rotationMatrix) < 0.0f) {
    controls.scale.x *= -1.0f;
    rotationMatrix[0] *= -1.0f;
  }

  controls.rotationDegrees =
      glm::degrees(glm::eulerAngles(glm::normalize(glm::quat_cast(rotationMatrix))));
  return controls;
}

glm::mat4 composeTransform(const utility::ui::TransformControls& controls) {
  const glm::vec3 safeScale = glm::max(controls.scale, glm::vec3(0.001f));
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), controls.position);
  transform *= glm::mat4_cast(glm::quat(glm::radians(controls.rotationDegrees)));
  transform = glm::scale(transform, safeScale);
  return transform;
}

glm::mat4 toShaderMatrix(const glm::mat4& columnVectorMatrix) {
  return glm::transpose(columnVectorMatrix);
}

glm::mat4 toShaderNormalMatrix(const glm::mat4& columnVectorModelMatrix) {
  // For CPU column-vector math the normal matrix is transpose(inverse(model)).
  // The shader upload path is transposed, so this becomes inverse(model).
  return glm::inverse(columnVectorModelMatrix);
}

}  // namespace


class HelloTriangleApplication {
 public:
  explicit HelloTriangleApplication(app::AppConfig config)
      : config_(std::move(config)) {
  }

  void run() {
    initWindow();
    initVulkan();
    mainLoop();
    cleanup();
  }

  ~HelloTriangleApplication() {
    // Ensure cleanup runs even if an exception escapes run().
    // cleanup() is idempotent (all handles are nulled after first call).
    if (deviceWrapper) {
      try { cleanup(); } catch (...) {}
    }
  }

 private:
  static constexpr float kDefaultCameraYaw = -135.0f;
  static constexpr float kDefaultCameraPitch = -35.0f;
  static constexpr float kDefaultCameraMoveSpeed = 1.0f;
  static constexpr float kDefaultCameraNearPlane = 0.05f;
  static constexpr float kDefaultCameraFarPlane = 500.0f;
  static constexpr uint32_t kExactOitAverageNodesPerPixel = 4u;

  app::AppConfig config_{};
  std::unique_ptr<window::WindowManager> windowManager;
  std::unique_ptr<window::Window> window;

  std::shared_ptr<utility::vulkan::VulkanInstance> instanceWrapper;
  std::shared_ptr<utility::vulkan::VulkanDevice> deviceWrapper;
  std::unique_ptr<utility::pipeline::PipelineManager> pipelineManager;

  VkInstance instance{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};
  VkSurfaceKHR surface{VK_NULL_HANDLE};

  std::unique_ptr<SwapChainManager> swapChainManager;

  VkRenderPass depthPrepassRenderPass{VK_NULL_HANDLE};
  VkRenderPass gBufferRenderPass{VK_NULL_HANDLE};
  VkRenderPass lightingRenderPass{VK_NULL_HANDLE};
  VkRenderPass postProcessRenderPass{VK_NULL_HANDLE};
  VkPipelineLayout scenePipelineLayout{VK_NULL_HANDLE};
  VkPipelineLayout transparentPipelineLayout{VK_NULL_HANDLE};
  VkPipelineLayout lightingPipelineLayout{VK_NULL_HANDLE};
  VkPipelineLayout postProcessPipelineLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout lightingDescriptorSetLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout postProcessDescriptorSetLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout oitDescriptorSetLayout{VK_NULL_HANDLE};
  VkDescriptorSetLayout lightDescriptorSetLayout{VK_NULL_HANDLE};
  VkDescriptorPool lightingDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorPool postProcessDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorPool oitDescriptorPool{VK_NULL_HANDLE};
  VkDescriptorPool lightDescriptorPool{VK_NULL_HANDLE};
  VkSampler gBufferSampler{VK_NULL_HANDLE};
  VkPipeline depthPrepassPipeline{VK_NULL_HANDLE};
  VkPipeline gBufferPipeline{VK_NULL_HANDLE};
  VkPipeline directionalLightPipeline{VK_NULL_HANDLE};
  VkPipeline stencilVolumePipeline{VK_NULL_HANDLE};
  VkPipeline stencilVolumePipelineFlipped{VK_NULL_HANDLE};
  VkPipeline pointLightPipeline{VK_NULL_HANDLE};
  VkPipeline pointLightStencilDebugPipeline{VK_NULL_HANDLE};
  VkPipeline transparentPipeline{VK_NULL_HANDLE};
  VkPipeline postProcessPipeline{VK_NULL_HANDLE};
  VkPipeline geometryDebugPipeline{VK_NULL_HANDLE};
  VkPipeline surfaceNormalLinePipeline{VK_NULL_HANDLE};
  VkPipeline lightGizmoPipeline{VK_NULL_HANDLE};
  VkFormat sceneColorFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat gBufferAlbedoFormat{VK_FORMAT_R8G8B8A8_UNORM};
  VkFormat gBufferNormalFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat gBufferMaterialFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat gBufferEmissiveFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat gBufferPositionFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
  VkFormat oitHeadPointerFormat{VK_FORMAT_R32_UINT};
  VkFormat depthStencilFormat{VK_FORMAT_UNDEFINED};
  std::vector<FrameResources> frameResources{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};

  VkCommandPool commandPool{VK_NULL_HANDLE};
  std::vector<VkCommandBuffer> commandBuffers;

  std::unique_ptr<utility::memory::AllocationManager> allocationManager;
  std::unique_ptr<utility::scene::SceneManager> sceneManager;
  std::unique_ptr<utility::ui::GuiManager> guiManager;
  utility::scene::SceneGraph sceneGraph{};
  std::vector<uint32_t> renderableNodes{};
  uint32_t rootNode = utility::scene::SceneGraph::kInvalidNode;
  uint32_t cubeNode = utility::scene::SceneGraph::kInvalidNode;
  uint32_t selectedMeshNode = utility::scene::SceneGraph::kInvalidNode;

  utility::memory::AllocatedBuffer cameraBuffer{};
  utility::memory::AllocatedBuffer objectBuffer{};
  utility::memory::AllocatedBuffer lightingBuffer{};
  size_t objectBufferCapacity{0};

  // Diagnostic cube: slices into the shared vertex/index arena.
  utility::memory::BufferSlice diagCubeVertexSlice{};
  utility::memory::BufferSlice diagCubeIndexSlice{};
  uint32_t diagCubeIndexCount{0};
  uint32_t diagCubeObjectIndex{std::numeric_limits<uint32_t>::max()};

  CameraData cameraData{};
  LightingData lightingData{};
  std::vector<ObjectData> objectData;
  std::vector<DrawCommand> opaqueDrawCommands;
  std::vector<DrawCommand> transparentDrawCommands;
  BindlessPushConstants pushConstants{};
  LightPushConstants lightPushConstants{};

  std::unique_ptr<utility::camera::BaseCamera> camera;
  utility::input::InputManager inputManager{};
  double lastFrameTimeSeconds{0.0};

  utility::memory::BufferSlice vertexSlice{};
  utility::memory::BufferSlice indexSlice{};
  utility::memory::BufferSlice lightVolumeVertexSlice{};
  utility::memory::BufferSlice lightVolumeIndexSlice{};
  uint32_t lightVolumeIndexCount{0};
  VkDescriptorSet lightDescriptorSet{VK_NULL_HANDLE};

  std::unique_ptr<FrameSyncManager> frameSyncManager;
  std::vector<VkFence> imagesInFlight;
  uint32_t currentFrame = 0;
  uint32_t exactOitNodeCapacityFloor{0};

  bool framebufferResized = false;
  bool debugDirectionalOnly = false;
  bool debugVisualizePointLightStencil = false;
  bool debugFlipPointLightFrontFace = false;
  bool debugDirectionalOnlyKeyDown = false;
  bool debugVisualizePointLightStencilKeyDown = false;
  bool debugFlipPointLightFrontFaceKeyDown = false;

  void initWindow() {
    windowManager = std::make_unique<window::WindowManager>();
    window = windowManager->createWindow(config_.windowWidth,
                                         config_.windowHeight, "Vulkan");
    GLFWwindow* nativeWindow = window->getNativeWindow();
    inputManager.setWindow(nativeWindow);
    glfwSetWindowUserPointer(nativeWindow, this);
    glfwSetFramebufferSizeCallback(nativeWindow, [](GLFWwindow* windowHandle,
                                                    int, int) {
      auto* app = static_cast<HelloTriangleApplication*>(
          glfwGetWindowUserPointer(windowHandle));
      if (app) {
        app->framebufferResized = true;
      }
    });
    glfwSetCursorPosCallback(
        nativeWindow, [](GLFWwindow* windowHandle, double xpos, double ypos) {
          auto* app = static_cast<HelloTriangleApplication*>(
              glfwGetWindowUserPointer(windowHandle));
          if (app) {
            app->inputManager.enqueueMouseDelta(xpos, ypos);
          }
        });
    glfwSetMouseButtonCallback(
        nativeWindow, [](GLFWwindow* windowHandle, int button, int action,
                         int) {
          auto* app = static_cast<HelloTriangleApplication*>(
              glfwGetWindowUserPointer(windowHandle));
          if (app) {
            app->inputManager.handleMouseButton(button, action);
          }
        });
    glfwSetKeyCallback(nativeWindow, [](GLFWwindow* windowHandle, int key,
                                        int, int action, int) {
      auto* app = static_cast<HelloTriangleApplication*>(
          glfwGetWindowUserPointer(windowHandle));
      if (app) {
        app->inputManager.handleKey(key, action);
      }
    });
    glfwSetWindowFocusCallback(nativeWindow, [](GLFWwindow* windowHandle,
                                                int focused) {
      auto* app = static_cast<HelloTriangleApplication*>(
          glfwGetWindowUserPointer(windowHandle));
      if (app) {
        app->inputManager.handleWindowFocus(focused == GLFW_TRUE);
      }
    });
  }

  std::vector<const char*> getRequiredExtensions() const {
    auto extensions = windowManager->getRequiredInstanceExtensions();
    if (config_.enableValidationLayers) {
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
  }

  void initVulkan() {
    createInstance();
    setupDebugMessenger();
    createSurface();
    createDevice();
    pipelineManager = std::make_unique<utility::pipeline::PipelineManager>(
        deviceWrapper->device());
    swapChainManager = std::make_unique<SwapChainManager>(
        window->getNativeWindow(), deviceWrapper->physicalDevice(),
        deviceWrapper->device(), surface);
    swapChainManager->initialize();
    createRenderPasses();
    createCommandPool();
    allocationManager = std::make_unique<utility::memory::AllocationManager>();
    allocationManager->initialize(
        instance, deviceWrapper->physicalDevice(), deviceWrapper->device(),
        deviceWrapper->graphicsQueue(), commandPool, config_);
    sceneManager = std::make_unique<utility::scene::SceneManager>(
        *allocationManager, *pipelineManager, deviceWrapper, config_);
    sceneManager->initialize(config_.modelPath);
    indexType = sceneManager->indexType();
    createLightingDescriptorSetLayout();
    createPostProcessDescriptorSetLayout();
    createOitDescriptorSetLayout();
    createLightDescriptorResources();
    createGBufferSampler();
    createGraphicsPipelines();
    swapChainManager->createFramebuffers(postProcessRenderPass);
    createCamera();
    buildSceneGraph();
    createSceneBuffers();
    updateLightingData();
    createLightVolumeGeometry();
    createFrameResources();
    createGeometryBuffers();
    sceneManager->updateDescriptorSet(cameraBuffer, objectBuffer);
    guiManager = std::make_unique<utility::ui::GuiManager>();
    guiManager->initialize(
        instance, deviceWrapper->device(), deviceWrapper->physicalDevice(),
        deviceWrapper->graphicsQueue(),
        deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
        postProcessRenderPass,
        static_cast<uint32_t>(swapChainManager->imageCount()),
        window->getNativeWindow(), config_.modelPath);
    createCommandBuffers();
    frameSyncManager = std::make_unique<FrameSyncManager>(
        deviceWrapper->device(), config_.maxFramesInFlight);
    frameSyncManager->initialize(
        static_cast<uint32_t>(swapChainManager->imageCount()));
    imagesInFlight.assign(swapChainManager->imageCount(), VK_NULL_HANDLE);
    utility::logger::ContainerLogger::instance().renderer()->info(
        "Initializing Vulkan renderer");
    utility::logger::ContainerLogger::instance().vulkan()->debug(
        "Debugging Vulkan initialization");
  }

  void mainLoop() {
    lastFrameTimeSeconds = windowManager->getTime();
    while (!window->shouldClose()) {
      const double currentTime = windowManager->getTime();
      const float deltaTime =
          static_cast<float>(currentTime - lastFrameTimeSeconds);
      lastFrameTimeSeconds = currentTime;

      window->pollEvents();
      processInput(deltaTime);
      drawFrame();
    }

    vkDeviceWaitIdle(deviceWrapper->device());
  }

  void cleanup() {
    if (swapChainManager) {
      swapChainManager->cleanup();
    }

    destroyGBufferResources();

    if (gBufferSampler != VK_NULL_HANDLE) {
      vkDestroySampler(deviceWrapper->device(), gBufferSampler, nullptr);
      gBufferSampler = VK_NULL_HANDLE;
    }

    if (guiManager) {
      guiManager->shutdown(deviceWrapper->device());
      guiManager.reset();
    }

    if (lightingRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(deviceWrapper->device(), lightingRenderPass, nullptr);
      lightingRenderPass = VK_NULL_HANDLE;
    }

    if (postProcessRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(deviceWrapper->device(), postProcessRenderPass,
                          nullptr);
      postProcessRenderPass = VK_NULL_HANDLE;
    }

    if (gBufferRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(deviceWrapper->device(), gBufferRenderPass, nullptr);
      gBufferRenderPass = VK_NULL_HANDLE;
    }

    if (depthPrepassRenderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(deviceWrapper->device(), depthPrepassRenderPass,
                          nullptr);
      depthPrepassRenderPass = VK_NULL_HANDLE;
    }

    if (sceneManager) {
      sceneManager.reset();
    }

    if (allocationManager) {
      if (cameraBuffer.buffer != VK_NULL_HANDLE) {
        allocationManager->destroyBuffer(cameraBuffer);
      }
      if (objectBuffer.buffer != VK_NULL_HANDLE) {
        allocationManager->destroyBuffer(objectBuffer);
      }
      if (lightingBuffer.buffer != VK_NULL_HANDLE) {
        allocationManager->destroyBuffer(lightingBuffer);
      }
      allocationManager->cleanup();
      allocationManager.reset();
    }

    if (pipelineManager) {
      pipelineManager->destroyManagedResources();
      depthPrepassPipeline = VK_NULL_HANDLE;
      gBufferPipeline = VK_NULL_HANDLE;
      directionalLightPipeline = VK_NULL_HANDLE;
      stencilVolumePipeline = VK_NULL_HANDLE;
      stencilVolumePipelineFlipped = VK_NULL_HANDLE;
      pointLightPipeline = VK_NULL_HANDLE;
      pointLightStencilDebugPipeline = VK_NULL_HANDLE;
      transparentPipeline = VK_NULL_HANDLE;
      postProcessPipeline = VK_NULL_HANDLE;
      geometryDebugPipeline = VK_NULL_HANDLE;
      surfaceNormalLinePipeline = VK_NULL_HANDLE;
      lightGizmoPipeline = VK_NULL_HANDLE;
      scenePipelineLayout = VK_NULL_HANDLE;
      transparentPipelineLayout = VK_NULL_HANDLE;
      lightingPipelineLayout = VK_NULL_HANDLE;
      postProcessPipelineLayout = VK_NULL_HANDLE;
      lightingDescriptorSetLayout = VK_NULL_HANDLE;
      postProcessDescriptorSetLayout = VK_NULL_HANDLE;
      oitDescriptorSetLayout = VK_NULL_HANDLE;
      lightDescriptorSetLayout = VK_NULL_HANDLE;
    }

    frameSyncManager.reset();

    if (commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(deviceWrapper->device(), commandPool, nullptr);
      commandPool = VK_NULL_HANDLE;
    }

    pipelineManager.reset();
    deviceWrapper.reset();

    if (config_.enableValidationLayers) {
      DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    }

    if (surface != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(instance, surface, nullptr);
      surface = VK_NULL_HANDLE;
    }
    instanceWrapper.reset();

    window.reset();
    windowManager.reset();
  }

  void recreateSwapChain() {
    int width = 0, height = 0;
    window->getFramebufferSize(width, height);
    while (width == 0 || height == 0) {
      window->getFramebufferSize(width, height);
      window->waitForEvents();
    }

    vkDeviceWaitIdle(deviceWrapper->device());

    if (swapChainManager) {
      destroyGBufferResources();
      swapChainManager->recreate(postProcessRenderPass);
      createFrameResources();
      recreateCommandBuffers();
      const uint32_t swapchainImageCount =
          static_cast<uint32_t>(swapChainManager->imageCount());
      if (guiManager) {
        guiManager->updateSwapchainImageCount(swapchainImageCount);
      }
      frameSyncManager->recreateRenderFinishedSemaphores(
          swapChainManager->imageCount());
      imagesInFlight.assign(swapChainManager->imageCount(), VK_NULL_HANDLE);
      updateCameraBuffer();
      updateObjectBuffer();
      updateFrameDescriptorSets();
    }
  }

  void createInstance() {
    utility::vulkan::InstanceCreateInfo createInfo{};
    createInfo.applicationName = "Hello Triangle";
    createInfo.engineName = "No Engine";
    createInfo.apiVersion = VK_API_VERSION_1_3;
    createInfo.enableValidationLayers = config_.enableValidationLayers;
    createInfo.validationLayers = config_.validationLayers;
    createInfo.requiredExtensions = getRequiredExtensions();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (config_.enableValidationLayers) {
      populateDebugMessengerCreateInfo(debugCreateInfo);
      createInfo.next = &debugCreateInfo;
    }

    instanceWrapper =
        std::make_shared<utility::vulkan::VulkanInstance>(createInfo);
    instance = instanceWrapper->instance();
  }

  void populateDebugMessengerCreateInfo(
      VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
  }

  void setupDebugMessenger() {
    if (!config_.enableValidationLayers) return;

    VkDebugUtilsMessengerCreateInfoEXT createInfo;
    populateDebugMessengerCreateInfo(createInfo);

    if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
                                     &debugMessenger) != VK_SUCCESS) {
      throw std::runtime_error("failed to set up debug messenger!");
    }
  }

  void createSurface() { surface = window->createSurface(instance); }

  void createDevice() {
    utility::vulkan::DeviceCreateInfo createInfo{};
    createInfo.requiredExtensions = config_.deviceExtensions;
    createInfo.validationLayers = config_.validationLayers;
    createInfo.enableValidationLayers = config_.enableValidationLayers;
    createInfo.enabledFeatures.samplerAnisotropy = VK_TRUE;
    createInfo.enabledFeatures.fragmentStoresAndAtomics = VK_TRUE;
    createInfo.enabledFeatures.geometryShader = VK_TRUE;

    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
    descriptorIndexingFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount =
        VK_TRUE;
    descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing =
        VK_TRUE;

    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
    bufferDeviceAddressFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceVulkan11Features vulkan11Features{};
    vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11Features.shaderDrawParameters = VK_TRUE;

    bufferDeviceAddressFeatures.pNext = &vulkan11Features;
    descriptorIndexingFeatures.pNext = &bufferDeviceAddressFeatures;
    createInfo.next = &descriptorIndexingFeatures;

    deviceWrapper = std::make_shared<utility::vulkan::VulkanDevice>(
        instance, surface, createInfo);
  }

  VkFormat findSupportedFormat(std::initializer_list<VkFormat> candidates,
                               VkImageTiling tiling,
                               VkFormatFeatureFlags features) const {
    for (VkFormat format : candidates) {
      VkFormatProperties properties{};
      vkGetPhysicalDeviceFormatProperties(deviceWrapper->physicalDevice(),
                                          format, &properties);

      const VkFormatFeatureFlags tilingFeatures =
          tiling == VK_IMAGE_TILING_LINEAR ? properties.linearTilingFeatures
                                           : properties.optimalTilingFeatures;
      if ((tilingFeatures & features) == features) {
        return format;
      }
    }

    throw std::runtime_error("failed to find a supported Vulkan image format");
  }

  VkFormat findDepthStencilFormat() const {
    return findSupportedFormat(
        {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
  }

  void validateExactOitFormatSupport() const {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(deviceWrapper->physicalDevice(),
                                        oitHeadPointerFormat, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ==
        0) {
      throw std::runtime_error(
          "selected GPU does not support R32_UINT storage images for exact OIT");
    }
  }

  AttachmentImage createAttachmentImage(VkFormat format,
                                        VkImageUsageFlags usage,
                                        VkImageAspectFlags aspectMask) {
    AttachmentImage attachment{};
    attachment.format = format;

    const VkExtent2D extent = swapChainManager->extent();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager->memoryManager()->allocator(),
                       &imageInfo, &allocationInfo, &attachment.image,
                       &attachment.allocation, nullptr) != VK_SUCCESS) {
      throw std::runtime_error("failed to create GBuffer attachment image");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = attachment.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(deviceWrapper->device(), &viewInfo, nullptr,
                          &attachment.view) != VK_SUCCESS) {
      vmaDestroyImage(allocationManager->memoryManager()->allocator(),
                      attachment.image, attachment.allocation);
      throw std::runtime_error("failed to create GBuffer attachment view");
    }

    return attachment;
  }

  VkCommandBuffer beginImmediateCommands() const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    if (vkAllocateCommandBuffers(deviceWrapper->device(), &allocInfo,
                                 &commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate immediate command buffer");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      vkFreeCommandBuffers(deviceWrapper->device(), commandPool, 1,
                           &commandBuffer);
      throw std::runtime_error("failed to begin immediate command buffer");
    }

    return commandBuffer;
  }

  void endImmediateCommands(VkCommandBuffer commandBuffer) const {
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      vkFreeCommandBuffers(deviceWrapper->device(), commandPool, 1,
                           &commandBuffer);
      throw std::runtime_error("failed to end immediate command buffer");
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    if (vkQueueSubmit(deviceWrapper->graphicsQueue(), 1, &submitInfo,
                      VK_NULL_HANDLE) != VK_SUCCESS) {
      vkFreeCommandBuffers(deviceWrapper->device(), commandPool, 1,
                           &commandBuffer);
      throw std::runtime_error("failed to submit immediate command buffer");
    }

    vkQueueWaitIdle(deviceWrapper->graphicsQueue());
    vkFreeCommandBuffers(deviceWrapper->device(), commandPool, 1,
                         &commandBuffer);
  }

  void transitionImageToGeneral(VkImage image,
                                VkImageAspectFlags aspectMask) const {
    VkCommandBuffer commandBuffer = beginImmediateCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    endImmediateCommands(commandBuffer);
  }

  void destroyAttachmentImage(AttachmentImage& attachment) {
    if (attachment.view != VK_NULL_HANDLE) {
      vkDestroyImageView(deviceWrapper->device(), attachment.view, nullptr);
      attachment.view = VK_NULL_HANDLE;
    }

    if (attachment.image != VK_NULL_HANDLE && attachment.allocation != nullptr) {
      vmaDestroyImage(allocationManager->memoryManager()->allocator(),
                      attachment.image, attachment.allocation);
    }

    attachment = {};
  }

  [[nodiscard]] uint32_t computeExactOitNodeCapacity() const {
    const VkExtent2D extent = swapChainManager->extent();
    const uint64_t pixelCount =
        static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height);
    const uint64_t desiredNodeCount =
        std::max<uint64_t>(1, pixelCount * kExactOitAverageNodesPerPixel);
    const uint64_t boundedDesiredNodeCount =
        std::min<uint64_t>(desiredNodeCount,
                           static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
    return std::max<uint32_t>(static_cast<uint32_t>(boundedDesiredNodeCount),
                              exactOitNodeCapacityFloor);
  }

  void writeExactOitMetadata(FrameResources& frame) {
    if (frame.oitMetadataBuffer.buffer == VK_NULL_HANDLE) {
      return;
    }

    const VkExtent2D extent = swapChainManager->extent();
    const ExactOitMetadata metadata{
        frame.oitNodeCapacity, extent.width, extent.height, 0u};
    writeToBuffer(frame.oitMetadataBuffer, &metadata, sizeof(metadata));
  }

  bool growExactOitNodePoolIfNeeded(uint32_t imageIndex) {
    if (imageIndex >= frameResources.size() || !allocationManager) {
      return false;
    }

    auto& frame = frameResources[imageIndex];
    if (frame.oitCounterBuffer.buffer == VK_NULL_HANDLE) {
      return false;
    }

    auto* mappedCounter = static_cast<const uint32_t*>(
        frame.oitCounterBuffer.allocation_info.pMappedData);
    if (mappedCounter == nullptr) {
      return false;
    }

    if (vmaInvalidateAllocation(allocationManager->memoryManager()->allocator(),
                                frame.oitCounterBuffer.allocation, 0,
                                sizeof(uint32_t)) != VK_SUCCESS) {
      throw std::runtime_error("failed to invalidate exact OIT counter buffer");
    }

    const uint32_t requiredNodeCount = mappedCounter[0];
    if (requiredNodeCount <= frame.oitNodeCapacity) {
      return false;
    }

    exactOitNodeCapacityFloor =
        std::max(requiredNodeCount, frame.oitNodeCapacity * 2u);
    vkDeviceWaitIdle(deviceWrapper->device());
    createFrameResources();
    if (guiManager) {
      guiManager->setStatusMessage(
          "Expanded exact OIT node pool to " +
          std::to_string(exactOitNodeCapacityFloor));
    }
    return true;
  }

  void createLightingDescriptorSetLayout() {
    if (lightingDescriptorSetLayout != VK_NULL_HANDLE) {
      return;
    }

    const std::array<VkDescriptorSetLayoutBinding, 7> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
    }};

    const std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(),
                                                             0);
    lightingDescriptorSetLayout = pipelineManager->createDescriptorSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                 bindings.end()),
        bindingFlags);
  }

  void createPostProcessDescriptorSetLayout() {
    if (postProcessDescriptorSetLayout != VK_NULL_HANDLE) {
      return;
    }

    const std::array<VkDescriptorSetLayoutBinding, 7> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
    }};

    const std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(),
                                                             0);
    postProcessDescriptorSetLayout = pipelineManager->createDescriptorSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                 bindings.end()),
        bindingFlags);
  }

  void createOitDescriptorSetLayout() {
    if (oitDescriptorSetLayout != VK_NULL_HANDLE) {
      return;
    }

    const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
    }};

    const std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(),
                                                             0);
    oitDescriptorSetLayout = pipelineManager->createDescriptorSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                 bindings.end()),
        bindingFlags);
  }

  void createLightDescriptorResources() {
    if (lightDescriptorSetLayout == VK_NULL_HANDLE) {
      const VkDescriptorSetLayoutBinding binding{
          0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
          nullptr};
      lightDescriptorSetLayout = pipelineManager->createDescriptorSetLayout(
          {binding}, {0});
    }

    if (lightingBuffer.buffer == VK_NULL_HANDLE) {
      lightingBuffer = allocationManager->createBuffer(
          sizeof(LightingData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    if (lightDescriptorPool == VK_NULL_HANDLE) {
      lightDescriptorPool = pipelineManager->createDescriptorPool(
          {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}, 1, 0);
    }

    if (lightDescriptorSet == VK_NULL_HANDLE) {
      VkDescriptorSetAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      allocInfo.descriptorPool = lightDescriptorPool;
      allocInfo.descriptorSetCount = 1;
      allocInfo.pSetLayouts = &lightDescriptorSetLayout;

      if (vkAllocateDescriptorSets(deviceWrapper->device(), &allocInfo,
                                   &lightDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate light descriptor set");
      }
    }

    VkDescriptorBufferInfo lightBufferInfo{lightingBuffer.buffer, 0,
                                           sizeof(LightingData)};
    VkWriteDescriptorSet lightWrite{};
    lightWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    lightWrite.dstSet = lightDescriptorSet;
    lightWrite.dstBinding = 0;
    lightWrite.descriptorCount = 1;
    lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightWrite.pBufferInfo = &lightBufferInfo;
    vkUpdateDescriptorSets(deviceWrapper->device(), 1, &lightWrite, 0,
                           nullptr);
  }

  void createGBufferSampler() {
    if (gBufferSampler != VK_NULL_HANDLE) {
      return;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;

    if (vkCreateSampler(deviceWrapper->device(), &samplerInfo, nullptr,
                        &gBufferSampler) != VK_SUCCESS) {
      throw std::runtime_error("failed to create GBuffer sampler");
    }
  }

  void destroyGBufferResources() {
    for (auto& frame : frameResources) {
      if (frame.depthPrepassFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(deviceWrapper->device(),
                             frame.depthPrepassFramebuffer, nullptr);
        frame.depthPrepassFramebuffer = VK_NULL_HANDLE;
      }
      if (frame.gBufferFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(deviceWrapper->device(), frame.gBufferFramebuffer,
                             nullptr);
        frame.gBufferFramebuffer = VK_NULL_HANDLE;
      }
      if (frame.lightingFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(deviceWrapper->device(), frame.lightingFramebuffer,
                             nullptr);
        frame.lightingFramebuffer = VK_NULL_HANDLE;
      }
      destroyAttachmentImage(frame.albedo);
      destroyAttachmentImage(frame.normal);
      destroyAttachmentImage(frame.material);
      destroyAttachmentImage(frame.emissive);
      destroyAttachmentImage(frame.position);
      destroyAttachmentImage(frame.depthStencil);
      destroyAttachmentImage(frame.sceneColor);
      destroyAttachmentImage(frame.oitHeadPointers);
      if (allocationManager) {
        allocationManager->destroyBuffer(frame.oitNodeBuffer);
        allocationManager->destroyBuffer(frame.oitCounterBuffer);
        allocationManager->destroyBuffer(frame.oitMetadataBuffer);
      }
      frame.oitDescriptorSet = VK_NULL_HANDLE;
      frame.oitNodeCapacity = 0;
      frame.lightingDescriptorSet = VK_NULL_HANDLE;
      frame.postProcessDescriptorSet = VK_NULL_HANDLE;
    }
    frameResources.clear();

    if (lightingDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(deviceWrapper->device(), lightingDescriptorPool,
                              nullptr);
      lightingDescriptorPool = VK_NULL_HANDLE;
    }

    if (postProcessDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(deviceWrapper->device(), postProcessDescriptorPool,
                              nullptr);
      postProcessDescriptorPool = VK_NULL_HANDLE;
    }

    if (oitDescriptorPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(deviceWrapper->device(), oitDescriptorPool,
                              nullptr);
      oitDescriptorPool = VK_NULL_HANDLE;
    }
  }

  void updateFrameDescriptorSets() {
    if (cameraBuffer.buffer == VK_NULL_HANDLE) {
      return;
    }

    for (auto& frame : frameResources) {
      VkDescriptorBufferInfo cameraInfo{cameraBuffer.buffer, 0,
                                        sizeof(CameraData)};

      VkDescriptorImageInfo samplerInfo{};
      samplerInfo.sampler = gBufferSampler;

      VkDescriptorImageInfo albedoInfo{};
      albedoInfo.imageView = frame.albedo.view;
      albedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkDescriptorImageInfo normalInfo{};
      normalInfo.imageView = frame.normal.view;
      normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkDescriptorImageInfo materialInfo{};
      materialInfo.imageView = frame.material.view;
      materialInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkDescriptorImageInfo emissiveInfo{};
      emissiveInfo.imageView = frame.emissive.view;
      emissiveInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkDescriptorImageInfo positionInfo{};
      positionInfo.imageView = frame.position.view;
      positionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      std::array<VkWriteDescriptorSet, 7> descriptorWrites{};
      descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[0].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[0].dstBinding = 0;
      descriptorWrites[0].descriptorCount = 1;
      descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      descriptorWrites[0].pBufferInfo = &cameraInfo;

      descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[1].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[1].dstBinding = 1;
      descriptorWrites[1].descriptorCount = 1;
      descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      descriptorWrites[1].pImageInfo = &samplerInfo;

      descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[2].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[2].dstBinding = 2;
      descriptorWrites[2].descriptorCount = 1;
      descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptorWrites[2].pImageInfo = &albedoInfo;

      descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[3].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[3].dstBinding = 3;
      descriptorWrites[3].descriptorCount = 1;
      descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptorWrites[3].pImageInfo = &normalInfo;

      descriptorWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[4].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[4].dstBinding = 4;
      descriptorWrites[4].descriptorCount = 1;
      descriptorWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptorWrites[4].pImageInfo = &materialInfo;

      descriptorWrites[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[5].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[5].dstBinding = 5;
      descriptorWrites[5].descriptorCount = 1;
      descriptorWrites[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptorWrites[5].pImageInfo = &emissiveInfo;

      descriptorWrites[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrites[6].dstSet = frame.lightingDescriptorSet;
      descriptorWrites[6].dstBinding = 6;
      descriptorWrites[6].descriptorCount = 1;
      descriptorWrites[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptorWrites[6].pImageInfo = &positionInfo;

      vkUpdateDescriptorSets(deviceWrapper->device(),
                             static_cast<uint32_t>(descriptorWrites.size()),
                             descriptorWrites.data(), 0, nullptr);

      VkDescriptorImageInfo sceneColorInfo{};
      sceneColorInfo.imageView = frame.sceneColor.view;
      sceneColorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      std::array<VkWriteDescriptorSet, 7> postWrites{};
      postWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[0].dstSet = frame.postProcessDescriptorSet;
      postWrites[0].dstBinding = 0;
      postWrites[0].descriptorCount = 1;
      postWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      postWrites[0].pImageInfo = &samplerInfo;

      postWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[1].dstSet = frame.postProcessDescriptorSet;
      postWrites[1].dstBinding = 1;
      postWrites[1].descriptorCount = 1;
      postWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      postWrites[1].pImageInfo = &sceneColorInfo;

      postWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[2].dstSet = frame.postProcessDescriptorSet;
      postWrites[2].dstBinding = 2;
      postWrites[2].descriptorCount = 1;
      postWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      postWrites[2].pImageInfo = &albedoInfo;

      postWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[3].dstSet = frame.postProcessDescriptorSet;
      postWrites[3].dstBinding = 3;
      postWrites[3].descriptorCount = 1;
      postWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      postWrites[3].pImageInfo = &normalInfo;

      postWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[4].dstSet = frame.postProcessDescriptorSet;
      postWrites[4].dstBinding = 4;
      postWrites[4].descriptorCount = 1;
      postWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      postWrites[4].pImageInfo = &materialInfo;

      postWrites[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[5].dstSet = frame.postProcessDescriptorSet;
      postWrites[5].dstBinding = 5;
      postWrites[5].descriptorCount = 1;
      postWrites[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      postWrites[5].pImageInfo = &emissiveInfo;

      postWrites[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      postWrites[6].dstSet = frame.postProcessDescriptorSet;
      postWrites[6].dstBinding = 6;
      postWrites[6].descriptorCount = 1;
      postWrites[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      postWrites[6].pImageInfo = &positionInfo;

      vkUpdateDescriptorSets(deviceWrapper->device(),
                             static_cast<uint32_t>(postWrites.size()),
                             postWrites.data(), 0, nullptr);

      VkDescriptorImageInfo headPointerInfo{};
      headPointerInfo.imageView = frame.oitHeadPointers.view;
      headPointerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkDescriptorBufferInfo nodeBufferInfo{
          frame.oitNodeBuffer.buffer, 0,
          sizeof(ExactOitNode) * frame.oitNodeCapacity};
      VkDescriptorBufferInfo counterBufferInfo{
          frame.oitCounterBuffer.buffer, 0, sizeof(uint32_t)};
      VkDescriptorBufferInfo metadataBufferInfo{
          frame.oitMetadataBuffer.buffer, 0, sizeof(ExactOitMetadata)};

      std::array<VkWriteDescriptorSet, 4> oitWrites{};
      oitWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      oitWrites[0].dstSet = frame.oitDescriptorSet;
      oitWrites[0].dstBinding = 0;
      oitWrites[0].descriptorCount = 1;
      oitWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      oitWrites[0].pImageInfo = &headPointerInfo;

      oitWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      oitWrites[1].dstSet = frame.oitDescriptorSet;
      oitWrites[1].dstBinding = 1;
      oitWrites[1].descriptorCount = 1;
      oitWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      oitWrites[1].pBufferInfo = &nodeBufferInfo;

      oitWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      oitWrites[2].dstSet = frame.oitDescriptorSet;
      oitWrites[2].dstBinding = 2;
      oitWrites[2].descriptorCount = 1;
      oitWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      oitWrites[2].pBufferInfo = &counterBufferInfo;

      oitWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      oitWrites[3].dstSet = frame.oitDescriptorSet;
      oitWrites[3].dstBinding = 3;
      oitWrites[3].descriptorCount = 1;
      oitWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      oitWrites[3].pBufferInfo = &metadataBufferInfo;

      vkUpdateDescriptorSets(deviceWrapper->device(),
                             static_cast<uint32_t>(oitWrites.size()),
                             oitWrites.data(), 0, nullptr);
    }
  }

  void createFrameResources() {
    destroyGBufferResources();
    validateExactOitFormatSupport();

    const uint32_t imageCount =
        static_cast<uint32_t>(swapChainManager->imageCount());
    if (imageCount == 0) {
      return;
    }

    std::array<VkDescriptorPoolSize, 3> lightingPoolSizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, imageCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount * 5},
    }};

    VkDescriptorPoolCreateInfo lightingPoolInfo{};
    lightingPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    lightingPoolInfo.maxSets = imageCount;
    lightingPoolInfo.poolSizeCount =
        static_cast<uint32_t>(lightingPoolSizes.size());
    lightingPoolInfo.pPoolSizes = lightingPoolSizes.data();

    if (vkCreateDescriptorPool(deviceWrapper->device(), &lightingPoolInfo,
                               nullptr, &lightingDescriptorPool) != VK_SUCCESS) {
      throw std::runtime_error("failed to create lighting descriptor pool");
    }

    std::array<VkDescriptorPoolSize, 2> postPoolSizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLER, imageCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount * 6},
    }};

    VkDescriptorPoolCreateInfo postPoolInfo{};
    postPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    postPoolInfo.maxSets = imageCount;
    postPoolInfo.poolSizeCount = static_cast<uint32_t>(postPoolSizes.size());
    postPoolInfo.pPoolSizes = postPoolSizes.data();

    if (vkCreateDescriptorPool(deviceWrapper->device(), &postPoolInfo, nullptr,
                               &postProcessDescriptorPool) != VK_SUCCESS) {
      throw std::runtime_error("failed to create post-process descriptor pool");
    }

    std::array<VkDescriptorPoolSize, 3> oitPoolSizes = {{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount},
    }};

    VkDescriptorPoolCreateInfo oitPoolInfo{};
    oitPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    oitPoolInfo.maxSets = imageCount;
    oitPoolInfo.poolSizeCount = static_cast<uint32_t>(oitPoolSizes.size());
    oitPoolInfo.pPoolSizes = oitPoolSizes.data();

    if (vkCreateDescriptorPool(deviceWrapper->device(), &oitPoolInfo, nullptr,
                               &oitDescriptorPool) != VK_SUCCESS) {
      throw std::runtime_error("failed to create exact OIT descriptor pool");
    }

    std::vector<VkDescriptorSetLayout> lightingLayouts(imageCount,
                                                       lightingDescriptorSetLayout);
    std::vector<VkDescriptorSetLayout> postLayouts(imageCount,
                                                   postProcessDescriptorSetLayout);
    std::vector<VkDescriptorSetLayout> oitLayouts(imageCount,
                                                  oitDescriptorSetLayout);
    std::vector<VkDescriptorSet> lightingSets(imageCount, VK_NULL_HANDLE);
    std::vector<VkDescriptorSet> postSets(imageCount, VK_NULL_HANDLE);
    std::vector<VkDescriptorSet> oitSets(imageCount, VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo lightingAllocInfo{};
    lightingAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    lightingAllocInfo.descriptorPool = lightingDescriptorPool;
    lightingAllocInfo.descriptorSetCount = imageCount;
    lightingAllocInfo.pSetLayouts = lightingLayouts.data();

    if (vkAllocateDescriptorSets(deviceWrapper->device(), &lightingAllocInfo,
                                 lightingSets.data()) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate lighting descriptor sets");
    }

    VkDescriptorSetAllocateInfo postAllocInfo{};
    postAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    postAllocInfo.descriptorPool = postProcessDescriptorPool;
    postAllocInfo.descriptorSetCount = imageCount;
    postAllocInfo.pSetLayouts = postLayouts.data();

    if (vkAllocateDescriptorSets(deviceWrapper->device(), &postAllocInfo,
                                 postSets.data()) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to allocate post-process descriptor sets");
    }

    VkDescriptorSetAllocateInfo oitAllocInfo{};
    oitAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    oitAllocInfo.descriptorPool = oitDescriptorPool;
    oitAllocInfo.descriptorSetCount = imageCount;
    oitAllocInfo.pSetLayouts = oitLayouts.data();

    if (vkAllocateDescriptorSets(deviceWrapper->device(), &oitAllocInfo,
                                 oitSets.data()) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate exact OIT descriptor sets");
    }

    frameResources.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
      auto& frame = frameResources[i];
      frame.lightingDescriptorSet = lightingSets[i];
      frame.postProcessDescriptorSet = postSets[i];
      frame.oitDescriptorSet = oitSets[i];
      frame.albedo =
          createAttachmentImage(gBufferAlbedoFormat,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      frame.normal =
          createAttachmentImage(gBufferNormalFormat,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      frame.material =
          createAttachmentImage(gBufferMaterialFormat,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      frame.emissive =
          createAttachmentImage(gBufferEmissiveFormat,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      frame.position =
          createAttachmentImage(gBufferPositionFormat,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      frame.sceneColor =
          createAttachmentImage(sceneColorFormat,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      frame.oitHeadPointers =
          createAttachmentImage(oitHeadPointerFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
      transitionImageToGeneral(frame.oitHeadPointers.image,
                               VK_IMAGE_ASPECT_COLOR_BIT);
      frame.oitNodeCapacity = computeExactOitNodeCapacity();
      frame.oitNodeBuffer = allocationManager->createBuffer(
          sizeof(ExactOitNode) * frame.oitNodeCapacity,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      frame.oitCounterBuffer = allocationManager->createBuffer(
          sizeof(uint32_t),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
      frame.oitMetadataBuffer = allocationManager->createBuffer(
          sizeof(ExactOitMetadata), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
      frame.depthStencil =
          createAttachmentImage(depthStencilFormat,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT |
                                    VK_IMAGE_ASPECT_STENCIL_BIT);
      writeExactOitMetadata(frame);

      VkFramebufferCreateInfo framebufferInfo{};
      framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      framebufferInfo.renderPass = depthPrepassRenderPass;
      framebufferInfo.attachmentCount = 1;
      framebufferInfo.pAttachments = &frame.depthStencil.view;
      framebufferInfo.width = swapChainManager->extent().width;
      framebufferInfo.height = swapChainManager->extent().height;
      framebufferInfo.layers = 1;

      if (vkCreateFramebuffer(deviceWrapper->device(), &framebufferInfo,
                              nullptr,
                              &frame.depthPrepassFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create depth prepass framebuffer");
      }

      std::array<VkImageView, 6> gBufferAttachments = {frame.albedo.view,
                                                       frame.normal.view,
                                                       frame.material.view,
                                                       frame.emissive.view,
                                                       frame.position.view,
                                                       frame.depthStencil.view};
      framebufferInfo.renderPass = gBufferRenderPass;
      framebufferInfo.attachmentCount =
          static_cast<uint32_t>(gBufferAttachments.size());
      framebufferInfo.pAttachments = gBufferAttachments.data();

      if (vkCreateFramebuffer(deviceWrapper->device(), &framebufferInfo,
                              nullptr, &frame.gBufferFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create GBuffer framebuffer");
      }

      std::array<VkImageView, 2> lightingAttachments = {frame.sceneColor.view,
                                                        frame.depthStencil.view};
      framebufferInfo.renderPass = lightingRenderPass;
      framebufferInfo.attachmentCount =
          static_cast<uint32_t>(lightingAttachments.size());
      framebufferInfo.pAttachments = lightingAttachments.data();
      if (vkCreateFramebuffer(deviceWrapper->device(), &framebufferInfo,
                              nullptr, &frame.lightingFramebuffer) !=
          VK_SUCCESS) {
        throw std::runtime_error("failed to create lighting framebuffer");
      }
    }

    updateFrameDescriptorSets();
  }

  void createRenderPasses() {
    depthStencilFormat = findDepthStencilFormat();

    VkAttachmentDescription depthStencilAttachment{};
    depthStencilAttachment.format = depthStencilFormat;
    depthStencilAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthStencilAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthStencilAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthStencilAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthStencilAttachment.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthStencilAttachmentRef{};
    depthStencilAttachmentRef.attachment = 0;
    depthStencilAttachmentRef.layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription depthPrepassSubpass{};
    depthPrepassSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    depthPrepassSubpass.pDepthStencilAttachment = &depthStencilAttachmentRef;

    std::array<VkSubpassDependency, 2> depthDependencies{};
    depthDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    depthDependencies[0].dstSubpass = 0;
    depthDependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    depthDependencies[0].srcAccessMask = 0;
    depthDependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    depthDependencies[0].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    depthDependencies[1].srcSubpass = 0;
    depthDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    depthDependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depthDependencies[1].srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthDependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    depthDependencies[1].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo depthRenderPassInfo{};
    depthRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    depthRenderPassInfo.attachmentCount = 1;
    depthRenderPassInfo.pAttachments = &depthStencilAttachment;
    depthRenderPassInfo.subpassCount = 1;
    depthRenderPassInfo.pSubpasses = &depthPrepassSubpass;
    depthRenderPassInfo.dependencyCount =
        static_cast<uint32_t>(depthDependencies.size());
    depthRenderPassInfo.pDependencies = depthDependencies.data();

    if (vkCreateRenderPass(deviceWrapper->device(), &depthRenderPassInfo,
                           nullptr, &depthPrepassRenderPass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create depth prepass render pass");
    }

    VkAttachmentDescription albedoAttachment{};
    albedoAttachment.format = gBufferAlbedoFormat;
    albedoAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    albedoAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    albedoAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    albedoAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    albedoAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    albedoAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    albedoAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription normalAttachment = albedoAttachment;
    normalAttachment.format = gBufferNormalFormat;

    VkAttachmentDescription materialAttachment = albedoAttachment;
    materialAttachment.format = gBufferMaterialFormat;

    VkAttachmentDescription emissiveAttachment = albedoAttachment;
    emissiveAttachment.format = gBufferEmissiveFormat;

    VkAttachmentDescription positionAttachment = albedoAttachment;
    positionAttachment.format = gBufferPositionFormat;

    VkAttachmentDescription gBufferDepthAttachment = depthStencilAttachment;
    gBufferDepthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    gBufferDepthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    gBufferDepthAttachment.initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkAttachmentDescription, 6> gBufferAttachments = {
        albedoAttachment, normalAttachment, materialAttachment,
        emissiveAttachment, positionAttachment, gBufferDepthAttachment};

    std::array<VkAttachmentReference, 5> colorAttachmentRefs = {{
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    }};

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 5;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription gBufferSubpass{};
    gBufferSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    gBufferSubpass.colorAttachmentCount =
        static_cast<uint32_t>(colorAttachmentRefs.size());
    gBufferSubpass.pColorAttachments = colorAttachmentRefs.data();
    gBufferSubpass.pDepthStencilAttachment = &depthAttachmentRef;

    std::array<VkSubpassDependency, 2> gBufferDependencies{};
    gBufferDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    gBufferDependencies[0].dstSubpass = 0;
    gBufferDependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    gBufferDependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    gBufferDependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    gBufferDependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    gBufferDependencies[1].srcSubpass = 0;
    gBufferDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    gBufferDependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    gBufferDependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    gBufferDependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    gBufferDependencies[1].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    VkRenderPassCreateInfo gBufferRenderPassInfo{};
    gBufferRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    gBufferRenderPassInfo.attachmentCount =
        static_cast<uint32_t>(gBufferAttachments.size());
    gBufferRenderPassInfo.pAttachments = gBufferAttachments.data();
    gBufferRenderPassInfo.subpassCount = 1;
    gBufferRenderPassInfo.pSubpasses = &gBufferSubpass;
    gBufferRenderPassInfo.dependencyCount =
        static_cast<uint32_t>(gBufferDependencies.size());
    gBufferRenderPassInfo.pDependencies = gBufferDependencies.data();

    if (vkCreateRenderPass(deviceWrapper->device(), &gBufferRenderPassInfo,
                           nullptr, &gBufferRenderPass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create GBuffer render pass");
    }

    VkAttachmentDescription lightingColorAttachment{};
    lightingColorAttachment.format = sceneColorFormat;
    lightingColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    lightingColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    lightingColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    lightingColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    lightingColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    lightingColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    lightingColorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription lightingDepthAttachment = depthStencilAttachment;
    lightingDepthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    lightingDepthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    lightingDepthAttachment.initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkAttachmentReference, 1> lightingColorAttachmentRefs = {{
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    }};

    VkAttachmentReference lightingDepthAttachmentRef{};
    lightingDepthAttachmentRef.attachment = 1;
    lightingDepthAttachmentRef.layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription lightingSubpass{};
    lightingSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    lightingSubpass.colorAttachmentCount =
        static_cast<uint32_t>(lightingColorAttachmentRefs.size());
    lightingSubpass.pColorAttachments = lightingColorAttachmentRefs.data();
    lightingSubpass.pDepthStencilAttachment = &lightingDepthAttachmentRef;

    std::array<VkAttachmentDescription, 2> lightingAttachments = {
        lightingColorAttachment, lightingDepthAttachment};

    std::array<VkSubpassDependency, 2> lightingDependencies{};
    lightingDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    lightingDependencies[0].dstSubpass = 0;
    lightingDependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    lightingDependencies[0].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    lightingDependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    lightingDependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    lightingDependencies[1].srcSubpass = 0;
    lightingDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    lightingDependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    lightingDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    lightingDependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    lightingDependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo lightingRenderPassInfo{};
    lightingRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    lightingRenderPassInfo.attachmentCount =
        static_cast<uint32_t>(lightingAttachments.size());
    lightingRenderPassInfo.pAttachments = lightingAttachments.data();
    lightingRenderPassInfo.subpassCount = 1;
    lightingRenderPassInfo.pSubpasses = &lightingSubpass;
    lightingRenderPassInfo.dependencyCount =
        static_cast<uint32_t>(lightingDependencies.size());
    lightingRenderPassInfo.pDependencies = lightingDependencies.data();

    if (vkCreateRenderPass(deviceWrapper->device(), &lightingRenderPassInfo,
                           nullptr, &lightingRenderPass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create lighting render pass");
    }

    VkAttachmentDescription swapchainAttachment{};
    swapchainAttachment.format = swapChainManager->imageFormat();
    swapchainAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    swapchainAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    swapchainAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    swapchainAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapchainAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference swapchainAttachmentRef{};
    swapchainAttachmentRef.attachment = 0;
    swapchainAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription postProcessSubpass{};
    postProcessSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    postProcessSubpass.colorAttachmentCount = 1;
    postProcessSubpass.pColorAttachments = &swapchainAttachmentRef;

    std::array<VkSubpassDependency, 2> postDependencies{};
    postDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    postDependencies[0].dstSubpass = 0;
    postDependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    postDependencies[0].srcAccessMask = 0;
    postDependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    postDependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    postDependencies[1].srcSubpass = 0;
    postDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    postDependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    postDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    postDependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    postDependencies[1].dstAccessMask = 0;

    VkRenderPassCreateInfo postProcessRenderPassInfo{};
    postProcessRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    postProcessRenderPassInfo.attachmentCount = 1;
    postProcessRenderPassInfo.pAttachments = &swapchainAttachment;
    postProcessRenderPassInfo.subpassCount = 1;
    postProcessRenderPassInfo.pSubpasses = &postProcessSubpass;
    postProcessRenderPassInfo.dependencyCount =
        static_cast<uint32_t>(postDependencies.size());
    postProcessRenderPassInfo.pDependencies = postDependencies.data();

    if (vkCreateRenderPass(deviceWrapper->device(), &postProcessRenderPassInfo,
                           nullptr, &postProcessRenderPass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create post-process render pass");
    }
  }

  void buildSceneGraph() {
    sceneManager->populateSceneGraph(sceneGraph);
    rootNode = utility::scene::SceneGraph::kInvalidNode;
    const uint32_t existingNodeCount =
        static_cast<uint32_t>(sceneGraph.nodeCount());
    if (existingNodeCount > 0) {
      rootNode = sceneGraph.createNode(glm::mat4(1.0f),
                                       sceneManager->defaultMaterialIndex(),
                                       false);
      for (uint32_t nodeIndex = 0; nodeIndex < existingNodeCount; ++nodeIndex) {
        const auto* node = sceneGraph.getNode(nodeIndex);
        if (node != nullptr &&
            node->parent == utility::scene::SceneGraph::kInvalidNode) {
          sceneGraph.setParent(nodeIndex, rootNode);
        }
      }
      sceneGraph.updateWorldTransforms();
    }
    renderableNodes.assign(sceneGraph.renderableNodes().begin(),
                           sceneGraph.renderableNodes().end());
    if (renderableNodes.empty()) {
      selectedMeshNode = utility::scene::SceneGraph::kInvalidNode;
    } else if (std::find(renderableNodes.begin(), renderableNodes.end(),
                         selectedMeshNode) == renderableNodes.end()) {
      selectedMeshNode = renderableNodes.front();
    }
  }

  glm::mat4 defaultTransformForNewObject() const {
    const float offset =
        static_cast<float>(sceneGraph.renderableNodes().size()) * 1.5f;
    return glm::translate(glm::mat4(1.0f), glm::vec3(offset, 0.0f, 0.0f));
  }

  void addSceneObject(const glm::mat4& transform) {
    if (sceneGraph.renderableNodes().size() >= config_.maxSceneObjects) {
      if (guiManager) {
        guiManager->setStatusMessage("Reached maximum scene object capacity");
      }
      return;
    }
    const uint32_t node = sceneGraph.createNode(
        transform, sceneManager->defaultMaterialIndex(), true);
    sceneGraph.setParent(node, rootNode);
    sceneGraph.updateWorldTransforms();
    updateObjectBuffer();
    if (guiManager) {
      guiManager->setStatusMessage("Added object to scene");
    }
  }

  void createGraphicsPipelines() {
    const auto loadModule = [this](const char* path) {
      const auto fileData = utility::file::readFile(path);
      return utility::vulkan::createShaderModule(deviceWrapper->device(),
                                                 fileData);
    };
    const auto makeShaderStage = [](VkShaderModule module,
                                    VkShaderStageFlagBits stage) {
      VkPipelineShaderStageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      info.stage = stage;
      info.module = module;
      switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
          info.pName = "vertMain";
          break;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
          info.pName = "fragMain";
          break;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
          info.pName = "geomMain";
          break;
        default:
          info.pName = "main";
          break;
      }
      return info;
    };

    VkShaderModule depthPrepassVertShaderModule =
        loadModule("spv_shaders/depth_prepass.vert.spv");
    VkShaderModule depthPrepassFragShaderModule =
        loadModule("spv_shaders/depth_prepass.frag.spv");
    VkShaderModule gBufferVertShaderModule =
        loadModule("spv_shaders/gbuffer.vert.spv");
    VkShaderModule gBufferFragShaderModule =
        loadModule("spv_shaders/gbuffer.frag.spv");
    VkShaderModule directionalVertShaderModule =
        loadModule("spv_shaders/deferred_directional.vert.spv");
    VkShaderModule directionalFragShaderModule =
        loadModule("spv_shaders/deferred_directional.frag.spv");
    VkShaderModule stencilVertShaderModule =
        loadModule("spv_shaders/light_stencil.vert.spv");
    VkShaderModule stencilFragShaderModule =
        loadModule("spv_shaders/light_stencil.frag.spv");
    VkShaderModule pointVertShaderModule =
        loadModule("spv_shaders/point_light.vert.spv");
    VkShaderModule pointFragShaderModule =
        loadModule("spv_shaders/point_light.frag.spv");
    VkShaderModule pointDebugVertShaderModule =
        loadModule("spv_shaders/point_light_stencil_debug.vert.spv");
    VkShaderModule pointDebugFragShaderModule =
        loadModule("spv_shaders/point_light_stencil_debug.frag.spv");
    VkShaderModule transparentVertShaderModule =
        loadModule("spv_shaders/forward_transparent.vert.spv");
    VkShaderModule transparentFragShaderModule =
        loadModule("spv_shaders/forward_transparent.frag.spv");
    VkShaderModule postProcessVertShaderModule =
        loadModule("spv_shaders/post_process.vert.spv");
    VkShaderModule postProcessFragShaderModule =
        loadModule("spv_shaders/post_process.frag.spv");
    VkShaderModule debugVertShaderModule =
        loadModule("spv_shaders/geometry_debug.vert.spv");
    VkShaderModule debugFragShaderModule =
        loadModule("spv_shaders/geometry_debug.frag.spv");
    VkShaderModule surfaceNormalsVertShaderModule =
        loadModule("spv_shaders/surface_normals.vert.spv");
    VkShaderModule surfaceNormalsGeomShaderModule =
        loadModule("spv_shaders/surface_normals.geom.spv");
    VkShaderModule surfaceNormalsFragShaderModule =
        loadModule("spv_shaders/surface_normals.frag.spv");
    VkShaderModule lightGizmoVertShaderModule =
        loadModule("spv_shaders/light_gizmo.vert.spv");
    VkShaderModule lightGizmoFragShaderModule =
        loadModule("spv_shaders/light_gizmo.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> depthPrepassShaderStages = {
        makeShaderStage(depthPrepassVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(depthPrepassFragShaderModule,
                        VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> gBufferShaderStages = {
        makeShaderStage(gBufferVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(gBufferFragShaderModule, VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> directionalShaderStages = {
        makeShaderStage(directionalVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(directionalFragShaderModule,
                        VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> stencilShaderStages = {
        makeShaderStage(stencilVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(stencilFragShaderModule, VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> pointShaderStages = {
        makeShaderStage(pointVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(pointFragShaderModule, VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> pointDebugShaderStages = {
        makeShaderStage(pointDebugVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(pointDebugFragShaderModule,
                        VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> transparentShaderStages = {
        makeShaderStage(transparentVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(transparentFragShaderModule,
                        VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> postProcessShaderStages = {
        makeShaderStage(postProcessVertShaderModule,
                        VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(postProcessFragShaderModule,
                        VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> debugShaderStages = {
        makeShaderStage(debugVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(debugFragShaderModule, VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 3>
        surfaceNormalShaderStages = {
            makeShaderStage(surfaceNormalsVertShaderModule,
                            VK_SHADER_STAGE_VERTEX_BIT),
            makeShaderStage(surfaceNormalsGeomShaderModule,
                            VK_SHADER_STAGE_GEOMETRY_BIT),
            makeShaderStage(surfaceNormalsFragShaderModule,
                            VK_SHADER_STAGE_FRAGMENT_BIT)};
    std::array<VkPipelineShaderStageCreateInfo, 2> lightGizmoShaderStages = {
        makeShaderStage(lightGizmoVertShaderModule, VK_SHADER_STAGE_VERTEX_BIT),
        makeShaderStage(lightGizmoFragShaderModule,
                        VK_SHADER_STAGE_FRAGMENT_BIT)};

    const auto bindingDescription = geometry::Vertex::bindingDescription();
    const auto attributeDescriptions = geometry::Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo sceneVertexInputInfo{};
    sceneVertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    sceneVertexInputInfo.vertexBindingDescriptionCount = 1;
    sceneVertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    sceneVertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    sceneVertexInputInfo.pVertexAttributeDescriptions =
        attributeDescriptions.data();

    VkPipelineVertexInputStateCreateInfo fullscreenVertexInputInfo{};
    fullscreenVertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo triangleAssembly{};
    triangleAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    triangleAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    triangleAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineInputAssemblyStateCreateInfo pointAssembly = triangleAssembly;
    pointAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineInputAssemblyStateCreateInfo lineAssembly = triangleAssembly;
    lineAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo sceneRasterizer{};
    sceneRasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    sceneRasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    sceneRasterizer.lineWidth = 1.0f;
    sceneRasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    sceneRasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    sceneRasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineRasterizationStateCreateInfo fullscreenRasterizer =
        sceneRasterizer;
    fullscreenRasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    VkPipelineRasterizationStateCreateInfo fullscreenRasterizerFlipped =
        fullscreenRasterizer;
    fullscreenRasterizerFlipped.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    std::array<VkPipelineColorBlendAttachmentState, 5>
        gBufferColorBlendAttachments = {colorBlendAttachment,
                                        colorBlendAttachment,
                                        colorBlendAttachment,
                                        colorBlendAttachment,
                                        colorBlendAttachment};

    VkPipelineColorBlendAttachmentState additiveBlendAttachment =
        colorBlendAttachment;
    additiveBlendAttachment.blendEnable = VK_TRUE;
    additiveBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    additiveBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    additiveBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    additiveBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    additiveBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    additiveBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendAttachmentState noColorWriteAttachment =
        colorBlendAttachment;
    noColorWriteAttachment.colorWriteMask = 0;

    std::array<VkPipelineColorBlendAttachmentState, 1>
        lightingColorBlendAttachments = {colorBlendAttachment};
    std::array<VkPipelineColorBlendAttachmentState, 1>
        additiveLightingAttachments = {additiveBlendAttachment};
    std::array<VkPipelineColorBlendAttachmentState, 1>
        stencilLightingAttachments = {noColorWriteAttachment};
    std::array<VkPipelineColorBlendAttachmentState, 1>
        transparentOitAttachments = {noColorWriteAttachment};
    std::array<VkPipelineColorBlendAttachmentState, 1>
        overlayLightingAttachments = {colorBlendAttachment};

    VkPipelineColorBlendStateCreateInfo noColorBlending{};
    noColorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkPipelineColorBlendStateCreateInfo gBufferColorBlending{};
    gBufferColorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    gBufferColorBlending.attachmentCount =
        static_cast<uint32_t>(gBufferColorBlendAttachments.size());
    gBufferColorBlending.pAttachments = gBufferColorBlendAttachments.data();

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount =
        static_cast<uint32_t>(lightingColorBlendAttachments.size());
    colorBlending.pAttachments = lightingColorBlendAttachments.data();

    VkPipelineColorBlendStateCreateInfo additiveColorBlending = colorBlending;
    additiveColorBlending.pAttachments = additiveLightingAttachments.data();

    VkPipelineColorBlendStateCreateInfo transparentColorBlending =
        colorBlending;
    transparentColorBlending.pAttachments = transparentOitAttachments.data();

    VkPipelineColorBlendStateCreateInfo stencilColorBlending = colorBlending;
    stencilColorBlending.pAttachments = stencilLightingAttachments.data();

    VkPipelineColorBlendStateCreateInfo overlayColorBlending = colorBlending;
    overlayColorBlending.pAttachments = overlayLightingAttachments.data();

    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineDepthStencilStateCreateInfo depthPrepassDepthStencil{};
    depthPrepassDepthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthPrepassDepthStencil.depthTestEnable = VK_TRUE;
    depthPrepassDepthStencil.depthWriteEnable = VK_TRUE;
    depthPrepassDepthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineDepthStencilStateCreateInfo gBufferDepthStencil =
        depthPrepassDepthStencil;
    gBufferDepthStencil.depthWriteEnable = VK_FALSE;
    gBufferDepthStencil.depthCompareOp = VK_COMPARE_OP_EQUAL;

    VkPipelineDepthStencilStateCreateInfo disabledDepthStencil{};
    disabledDepthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    // Surface normal lines: depth test enabled (GREATER_OR_EQUAL for reverse-Z),
    // no depth write so lines don't occlude subsequent draws.
    VkPipelineDepthStencilStateCreateInfo normalLineDepthStencil =
        depthPrepassDepthStencil;
    normalLineDepthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo stencilDepthStencil =
        depthPrepassDepthStencil;
    stencilDepthStencil.depthWriteEnable = VK_FALSE;
    stencilDepthStencil.stencilTestEnable = VK_TRUE;
    stencilDepthStencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                 VK_STENCIL_OP_DECREMENT_AND_WRAP,
                                 VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
    stencilDepthStencil.back = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};

    VkPipelineDepthStencilStateCreateInfo pointLightDepthStencil =
        disabledDepthStencil;
    pointLightDepthStencil.stencilTestEnable = VK_TRUE;
    pointLightDepthStencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                    VK_STENCIL_OP_KEEP,
                                    VK_COMPARE_OP_NOT_EQUAL, 0xff, 0x00, 0};
    pointLightDepthStencil.back = pointLightDepthStencil.front;

    VkPipelineDepthStencilStateCreateInfo transparentDepthStencil =
        depthPrepassDepthStencil;
    transparentDepthStencil.depthWriteEnable = VK_FALSE;

    VkPushConstantRange scenePushConstantRange{};
    scenePushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    scenePushConstantRange.size = sizeof(BindlessPushConstants);

    VkPushConstantRange lightPushConstantRange{};
    lightPushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    lightPushConstantRange.size = sizeof(LightPushConstants);

    VkPushConstantRange postProcessPushConstantRange{};
    postProcessPushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    postProcessPushConstantRange.size = sizeof(PostProcessPushConstants);

    scenePipelineLayout = pipelineManager->createPipelineLayout(
        {sceneManager->descriptorSetLayout()}, {scenePushConstantRange});
    transparentPipelineLayout = pipelineManager->createPipelineLayout(
        {sceneManager->descriptorSetLayout(), lightDescriptorSetLayout,
         oitDescriptorSetLayout},
        {scenePushConstantRange});
    lightingPipelineLayout = pipelineManager->createPipelineLayout(
        {lightingDescriptorSetLayout, lightDescriptorSetLayout},
        {lightPushConstantRange});
    postProcessPipelineLayout = pipelineManager->createPipelineLayout(
        {postProcessDescriptorSetLayout, oitDescriptorSetLayout},
        {postProcessPushConstantRange});

    VkGraphicsPipelineCreateInfo depthPrepassPipelineInfo{};
    depthPrepassPipelineInfo.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    depthPrepassPipelineInfo.stageCount =
        static_cast<uint32_t>(depthPrepassShaderStages.size());
    depthPrepassPipelineInfo.pStages = depthPrepassShaderStages.data();
    depthPrepassPipelineInfo.pVertexInputState = &sceneVertexInputInfo;
    depthPrepassPipelineInfo.pInputAssemblyState = &triangleAssembly;
    depthPrepassPipelineInfo.pViewportState = &viewportState;
    depthPrepassPipelineInfo.pRasterizationState = &sceneRasterizer;
    depthPrepassPipelineInfo.pMultisampleState = &multisampling;
    depthPrepassPipelineInfo.pDepthStencilState = &depthPrepassDepthStencil;
    depthPrepassPipelineInfo.pColorBlendState = &noColorBlending;
    depthPrepassPipelineInfo.pDynamicState = &dynamicState;
    depthPrepassPipelineInfo.layout = scenePipelineLayout;
    depthPrepassPipelineInfo.renderPass = depthPrepassRenderPass;

    depthPrepassPipeline = pipelineManager->createGraphicsPipeline(
        depthPrepassPipelineInfo, "depth_prepass_pipeline");

    VkGraphicsPipelineCreateInfo gBufferPipelineInfo = depthPrepassPipelineInfo;
    gBufferPipelineInfo.stageCount =
        static_cast<uint32_t>(gBufferShaderStages.size());
    gBufferPipelineInfo.pStages = gBufferShaderStages.data();
    gBufferPipelineInfo.pDepthStencilState = &gBufferDepthStencil;
    gBufferPipelineInfo.pColorBlendState = &gBufferColorBlending;
    gBufferPipelineInfo.renderPass = gBufferRenderPass;

    gBufferPipeline = pipelineManager->createGraphicsPipeline(
        gBufferPipelineInfo, "gbuffer_pipeline");

    VkGraphicsPipelineCreateInfo fullscreenPipelineInfo{};
    fullscreenPipelineInfo.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    fullscreenPipelineInfo.stageCount =
        static_cast<uint32_t>(directionalShaderStages.size());
    fullscreenPipelineInfo.pStages = directionalShaderStages.data();
    fullscreenPipelineInfo.pVertexInputState = &fullscreenVertexInputInfo;
    fullscreenPipelineInfo.pInputAssemblyState = &triangleAssembly;
    fullscreenPipelineInfo.pViewportState = &viewportState;
    fullscreenPipelineInfo.pRasterizationState = &fullscreenRasterizer;
    fullscreenPipelineInfo.pMultisampleState = &multisampling;
    fullscreenPipelineInfo.pDepthStencilState = &disabledDepthStencil;
    fullscreenPipelineInfo.pColorBlendState = &colorBlending;
    fullscreenPipelineInfo.pDynamicState = &dynamicState;
    fullscreenPipelineInfo.layout = lightingPipelineLayout;
    fullscreenPipelineInfo.renderPass = lightingRenderPass;

    directionalLightPipeline = pipelineManager->createGraphicsPipeline(
        fullscreenPipelineInfo, "directional_light_pipeline");

    VkGraphicsPipelineCreateInfo stencilPipelineInfo = fullscreenPipelineInfo;
    stencilPipelineInfo.stageCount =
        static_cast<uint32_t>(stencilShaderStages.size());
    stencilPipelineInfo.pStages = stencilShaderStages.data();
    stencilPipelineInfo.pDepthStencilState = &stencilDepthStencil;
    stencilPipelineInfo.pColorBlendState = &stencilColorBlending;
    stencilVolumePipeline = pipelineManager->createGraphicsPipeline(
        stencilPipelineInfo, "stencil_volume_pipeline");

    VkGraphicsPipelineCreateInfo stencilPipelineInfoFlipped =
        stencilPipelineInfo;
    stencilPipelineInfoFlipped.pRasterizationState =
        &fullscreenRasterizerFlipped;
    stencilVolumePipelineFlipped = pipelineManager->createGraphicsPipeline(
        stencilPipelineInfoFlipped, "stencil_volume_pipeline_flipped");

    VkGraphicsPipelineCreateInfo pointPipelineInfo = fullscreenPipelineInfo;
    pointPipelineInfo.stageCount =
        static_cast<uint32_t>(pointShaderStages.size());
    pointPipelineInfo.pStages = pointShaderStages.data();
    pointPipelineInfo.pDepthStencilState = &pointLightDepthStencil;
    pointPipelineInfo.pColorBlendState = &additiveColorBlending;
    pointLightPipeline = pipelineManager->createGraphicsPipeline(
        pointPipelineInfo, "point_light_pipeline");

    VkGraphicsPipelineCreateInfo pointDebugPipelineInfo = pointPipelineInfo;
    pointDebugPipelineInfo.stageCount =
        static_cast<uint32_t>(pointDebugShaderStages.size());
    pointDebugPipelineInfo.pStages = pointDebugShaderStages.data();
    pointDebugPipelineInfo.pColorBlendState = &colorBlending;
    pointLightStencilDebugPipeline = pipelineManager->createGraphicsPipeline(
        pointDebugPipelineInfo, "point_light_stencil_debug_pipeline");

    VkGraphicsPipelineCreateInfo transparentPipelineInfo{};
    transparentPipelineInfo.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    transparentPipelineInfo.stageCount =
        static_cast<uint32_t>(transparentShaderStages.size());
    transparentPipelineInfo.pStages = transparentShaderStages.data();
    transparentPipelineInfo.pVertexInputState = &sceneVertexInputInfo;
    transparentPipelineInfo.pInputAssemblyState = &triangleAssembly;
    transparentPipelineInfo.pViewportState = &viewportState;
    transparentPipelineInfo.pRasterizationState = &sceneRasterizer;
    transparentPipelineInfo.pMultisampleState = &multisampling;
    transparentPipelineInfo.pDepthStencilState = &transparentDepthStencil;
    transparentPipelineInfo.pColorBlendState = &transparentColorBlending;
    transparentPipelineInfo.pDynamicState = &dynamicState;
    transparentPipelineInfo.layout = transparentPipelineLayout;
    transparentPipelineInfo.renderPass = lightingRenderPass;
    transparentPipeline = pipelineManager->createGraphicsPipeline(
        transparentPipelineInfo, "transparent_pipeline");

    VkGraphicsPipelineCreateInfo postProcessPipelineInfo =
        fullscreenPipelineInfo;
    postProcessPipelineInfo.stageCount =
        static_cast<uint32_t>(postProcessShaderStages.size());
    postProcessPipelineInfo.pStages = postProcessShaderStages.data();
    postProcessPipelineInfo.layout = postProcessPipelineLayout;
    postProcessPipelineInfo.renderPass = postProcessRenderPass;
    postProcessPipeline = pipelineManager->createGraphicsPipeline(
        postProcessPipelineInfo, "post_process_pipeline");

    VkGraphicsPipelineCreateInfo debugPipelineInfo = transparentPipelineInfo;
    debugPipelineInfo.stageCount =
        static_cast<uint32_t>(debugShaderStages.size());
    debugPipelineInfo.pStages = debugShaderStages.data();
    debugPipelineInfo.pInputAssemblyState = &pointAssembly;
    debugPipelineInfo.pColorBlendState = &overlayColorBlending;
    debugPipelineInfo.pDepthStencilState = &disabledDepthStencil;
    debugPipelineInfo.layout = scenePipelineLayout;
    geometryDebugPipeline = pipelineManager->createGraphicsPipeline(
        debugPipelineInfo, "geometry_debug_pipeline");

    VkGraphicsPipelineCreateInfo surfaceNormalPipelineInfo =
        transparentPipelineInfo;
    surfaceNormalPipelineInfo.stageCount =
        static_cast<uint32_t>(surfaceNormalShaderStages.size());
    surfaceNormalPipelineInfo.pStages = surfaceNormalShaderStages.data();
    surfaceNormalPipelineInfo.pInputAssemblyState = &triangleAssembly;
    surfaceNormalPipelineInfo.pColorBlendState = &overlayColorBlending;
    surfaceNormalPipelineInfo.pDepthStencilState = &normalLineDepthStencil;
    surfaceNormalPipelineInfo.layout = scenePipelineLayout;
    surfaceNormalLinePipeline = pipelineManager->createGraphicsPipeline(
        surfaceNormalPipelineInfo, "surface_normal_line_pipeline");

    VkGraphicsPipelineCreateInfo lightGizmoPipelineInfo =
        fullscreenPipelineInfo;
    lightGizmoPipelineInfo.stageCount =
        static_cast<uint32_t>(lightGizmoShaderStages.size());
    lightGizmoPipelineInfo.pStages = lightGizmoShaderStages.data();
    lightGizmoPipelineInfo.pVertexInputState = &fullscreenVertexInputInfo;
    lightGizmoPipelineInfo.pInputAssemblyState = &lineAssembly;
    lightGizmoPipelineInfo.pColorBlendState = &overlayColorBlending;
    lightGizmoPipelineInfo.pDepthStencilState = &disabledDepthStencil;
    lightGizmoPipelineInfo.layout = lightingPipelineLayout;
    lightGizmoPipelineInfo.renderPass = lightingRenderPass;
    lightGizmoPipeline = pipelineManager->createGraphicsPipeline(
        lightGizmoPipelineInfo, "light_gizmo_pipeline");

    const std::array<VkShaderModule, 23> shaderModules = {
        depthPrepassVertShaderModule, depthPrepassFragShaderModule,
        gBufferVertShaderModule,      gBufferFragShaderModule,
        directionalVertShaderModule,  directionalFragShaderModule,
        stencilVertShaderModule,      stencilFragShaderModule,
        pointVertShaderModule,        pointFragShaderModule,
        pointDebugVertShaderModule,   pointDebugFragShaderModule,
        transparentVertShaderModule,  transparentFragShaderModule,
        postProcessVertShaderModule,  postProcessFragShaderModule,
        debugVertShaderModule,        debugFragShaderModule,
        surfaceNormalsVertShaderModule, surfaceNormalsGeomShaderModule,
        surfaceNormalsFragShaderModule,
        lightGizmoVertShaderModule,   lightGizmoFragShaderModule};
    for (VkShaderModule module : shaderModules) {
      vkDestroyShaderModule(deviceWrapper->device(), module, nullptr);
    }
  }

  void createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = deviceWrapper->queueFamilyIndices();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if (vkCreateCommandPool(deviceWrapper->device(), &poolInfo, nullptr,
                            &commandPool) != VK_SUCCESS) {
      throw std::runtime_error("failed to create command pool!");
    }
  }

  bool ensureObjectBufferCapacity(size_t requiredObjectCount) {
    const size_t requiredCapacity = std::max<size_t>(1, requiredObjectCount);
    if (objectBuffer.buffer != VK_NULL_HANDLE &&
        objectBufferCapacity >= requiredCapacity) {
      return false;
    }

    if (objectBuffer.buffer != VK_NULL_HANDLE) {
      allocationManager->destroyBuffer(objectBuffer);
      objectBufferCapacity = 0;
    }

    objectBuffer = allocationManager->createBuffer(
        sizeof(ObjectData) * requiredCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    objectBufferCapacity = requiredCapacity;
    return true;
  }

  void createSceneBuffers() {
    if (cameraBuffer.buffer == VK_NULL_HANDLE) {
      cameraBuffer = allocationManager->createBuffer(
          sizeof(CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    ensureObjectBufferCapacity(sceneGraph.renderableNodes().size());

    updateCameraBuffer();
    updateObjectBuffer();
    updateFrameDescriptorSets();
  }

  void createCamera() {
    auto perspective = std::make_unique<utility::camera::PerspectiveCamera>();
    camera = std::move(perspective);
    inputManager.setCamera(camera.get());
    inputManager.setMouseSensitivity(0.10f);
    resetCameraForScene();
  }

  void resetCameraForScene() {
    if (!camera) return;
    camera->setScale(glm::vec3(1.0f));
    inputManager.setMoveSpeed(kDefaultCameraMoveSpeed);

    auto* perspective =
        dynamic_cast<utility::camera::PerspectiveCamera*>(camera.get());
    if (perspective) {
      float farPlane = kDefaultCameraFarPlane;
      bool hasBounds = false;
      glm::vec3 boundsCenter{0.0f};
      float boundsRadius = 1.0f;
      if (sceneManager) {
        const auto& bounds = sceneManager->modelBounds();
        if (bounds.valid) {
          hasBounds = true;
          boundsCenter = bounds.center;
          boundsRadius = bounds.radius;
          farPlane = std::max(farPlane,
                              glm::length(boundsCenter) + boundsRadius * 4.0f);
        }
      }
      perspective->setNearFar(kDefaultCameraNearPlane, farPlane);

      if (hasBounds) {
        camera->setYawPitch(kDefaultCameraYaw, kDefaultCameraPitch);
        // Place the camera outside the scene: pull back along the front vector
        // so that boundsCenter is in view at a comfortable distance.
        const float distance = boundsRadius * 2.5f + kDefaultCameraNearPlane;
        const glm::vec3 front = camera->frontVector();
        camera->setPosition(boundsCenter - front * distance);
        inputManager.setMoveSpeed(std::max(kDefaultCameraMoveSpeed,
                                           boundsRadius * 0.5f));
      } else {
        // No scene geometry: position camera outside the origin to view the
        // diagnostic cube at (0,0,0). In RH, yaw=90 gives front=(0,0,-1).
        camera->setYawPitch(90.0f, 0.0f);
        camera->setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
      }
    } else {
      camera->setYawPitch(kDefaultCameraYaw, kDefaultCameraPitch);
      camera->setPosition(glm::vec3(0.0f));
    }
  }

  utility::ui::TransformControls cameraTransformControls() const {
    utility::ui::TransformControls controls{};
    if (!camera) {
      return controls;
    }
    controls.position = camera->position();
    controls.rotationDegrees = {camera->pitchDegrees(), camera->yawDegrees(),
                                0.0f};
    controls.scale = camera->scale();
    return controls;
  }

  utility::ui::TransformControls nodeTransformControls(uint32_t nodeIndex) const {
    if (const auto* node = sceneGraph.getNode(nodeIndex)) {
      return decomposeTransform(node->localTransform);
    }
    return {};
  }

  void applyCameraTransform(const utility::ui::TransformControls& controls) {
    if (!camera) {
      return;
    }
    camera->setPosition(controls.position);
    camera->setYawPitch(controls.rotationDegrees.y, controls.rotationDegrees.x);
    camera->setScale(controls.scale);
    updateCameraBuffer();
  }

  void applyNodeTransform(
      uint32_t nodeIndex, const utility::ui::TransformControls& controls) {
    if (sceneGraph.getNode(nodeIndex) == nullptr) {
      return;
    }
    sceneGraph.setLocalTransform(nodeIndex, composeTransform(controls));
    sceneGraph.updateWorldTransforms();
    if (nodeIndex == rootNode) {
      updateLightingData();
    }
    updateObjectBuffer();
  }

  void selectMeshNode(uint32_t nodeIndex) {
    selectedMeshNode = sceneGraph.getNode(nodeIndex) != nullptr
                           ? nodeIndex
                           : utility::scene::SceneGraph::kInvalidNode;
  }

  SceneLightingAnchor computeSceneLightingAnchor() const {
    SceneLightingAnchor anchor{};
    if (!sceneManager) {
      return anchor;
    }

    const auto& bounds = sceneManager->modelBounds();
    if (const auto* root = sceneGraph.getNode(rootNode)) {
      anchor.sceneTransform = root->worldTransform;
    }

    const glm::vec3 localCenter =
        bounds.valid ? bounds.center : glm::vec3(0.0f);
    anchor.center =
        glm::vec3(anchor.sceneTransform * glm::vec4(localCenter, 1.0f));

    const float sceneScale = std::max(
        {glm::length(glm::vec3(anchor.sceneTransform[0])),
         glm::length(glm::vec3(anchor.sceneTransform[1])),
         glm::length(glm::vec3(anchor.sceneTransform[2])), 1.0f});
    anchor.localRadius =
        std::max(bounds.valid ? bounds.radius : 10.0f, 1.0f);
    anchor.worldRadius = anchor.localRadius * sceneScale;
    return anchor;
  }

  glm::vec3 directionalLightPosition() const {
    const SceneLightingAnchor anchor = computeSceneLightingAnchor();
    return anchor.center - glm::vec3(lightingData.directionalDirection) *
                               (anchor.worldRadius * 1.15f);
  }

  void updateLightingData() {
    const SceneLightingAnchor anchor = computeSceneLightingAnchor();
    const glm::mat4& sceneTransform = anchor.sceneTransform;
    const glm::vec3& center = anchor.center;
    const float localRadius = anchor.localRadius;
    const float radius = anchor.worldRadius;

    lightingData = {};
    const glm::vec3 baseDirectionalDirection =
        glm::normalize(glm::vec3(-0.45f, -1.0f, -0.3f));
    lightingData.directionalDirection = glm::vec4(
        glm::normalize(
            glm::vec3(sceneTransform * glm::vec4(baseDirectionalDirection, 0.0f))),
        0.0f);
    constexpr float kDirectionalIntensity = 1.75f;
    constexpr float kPointLightIntensity = 6.0f;
    constexpr float kPointLightRadiusScale = 0.5f;
    lightingData.directionalColorIntensity =
        glm::vec4(1.0f, 0.96f, 0.9f, kDirectionalIntensity);

    lightingData.pointLightCount = kMaxDeferredPointLights;
    const std::array<glm::vec3, kMaxDeferredPointLights> localPositions = {{
        {0.0f, 3.0f, 5.0f},
        {0.0f, 3.0f, -5.0f},
        {5.0f, 3.0f, 5.0f},
        {-5.0f, 3.0f, 5.0f},
    }};
    const std::array<glm::vec3, kMaxDeferredPointLights> colors = {{
        {1.0f, 0.78f, 0.58f},
        {0.55f, 0.7f, 1.0f},
        {1.0f, 0.58f, 0.62f},
        {0.72f, 1.0f, 0.7f},
    }};

    for (uint32_t i = 0; i < lightingData.pointLightCount; ++i) {
      const glm::vec3 worldPosition =
          glm::vec3(sceneTransform * glm::vec4(localPositions[i], 1.0f));
      lightingData.pointLights[i].positionRadius =
          glm::vec4(worldPosition, radius * kPointLightRadiusScale);
      lightingData.pointLights[i].colorIntensity =
          glm::vec4(colors[i], kPointLightIntensity);
    }

    if (lightingBuffer.buffer != VK_NULL_HANDLE) {
      writeToBuffer(lightingBuffer, &lightingData, sizeof(LightingData));
    }
  }

  void drawLightGizmos(
      VkCommandBuffer commandBuffer,
      const std::array<VkDescriptorSet, 2>& lightingDescriptorSets) {
    if (lightGizmoPipeline == VK_NULL_HANDLE) {
      return;
    }

    const SceneLightingAnchor anchor = computeSceneLightingAnchor();
    const glm::vec3 cameraPosition = camera ? camera->position() : anchor.center;
    const auto computeGizmoExtent =
        [&](const glm::vec3& worldPosition, float radiusBias) {
          const float distanceToCamera =
              glm::max(glm::length(worldPosition - cameraPosition), 0.1f);
          return std::clamp(distanceToCamera * 0.03f + radiusBias, 0.25f, 6.0f);
        };

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      lightGizmoPipeline);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipelineLayout,
        0, static_cast<uint32_t>(lightingDescriptorSets.size()),
        lightingDescriptorSets.data(), 0, nullptr);

    LightPushConstants gizmoPushConstants{};
    glm::vec3 directionalColor =
        glm::vec3(lightingData.directionalColorIntensity);
    const float directionalMaxChannel = std::max(
        {directionalColor.r, directionalColor.g, directionalColor.b, 0.0001f});
    directionalColor /= directionalMaxChannel;
    directionalColor = glm::mix(glm::max(directionalColor, glm::vec3(0.35f)),
                                glm::vec3(1.0f, 0.95f, 0.35f), 0.5f);
    const glm::vec3 directionalGizmoPosition =
        anchor.center - glm::vec3(lightingData.directionalDirection) *
                            (anchor.worldRadius * 1.15f);
    const float directionalGizmoExtent =
        computeGizmoExtent(directionalGizmoPosition,
                           std::clamp(anchor.worldRadius * 0.02f, 0.05f, 1.0f));
    gizmoPushConstants.positionRadius = glm::vec4(
        directionalGizmoPosition, directionalGizmoExtent);
    gizmoPushConstants.colorIntensity = glm::vec4(directionalColor, 1.0f);
    vkCmdPushConstants(commandBuffer, lightingPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(LightPushConstants), &gizmoPushConstants);
    vkCmdDraw(commandBuffer, 6, 1, 0, 0);

    const uint32_t pointLightCount =
        std::min(lightingData.pointLightCount, kMaxDeferredPointLights);
    for (uint32_t i = 0; i < pointLightCount; ++i) {
      glm::vec3 lightColor = glm::vec3(lightingData.pointLights[i].colorIntensity);
      const float maxChannel =
          std::max({lightColor.r, lightColor.g, lightColor.b, 0.0001f});
      lightColor /= maxChannel;
      lightColor = glm::mix(glm::max(lightColor, glm::vec3(0.35f)),
                            glm::vec3(1.0f), 0.35f);

      const glm::vec3 lightPosition =
          glm::vec3(lightingData.pointLights[i].positionRadius);
      const float pointGizmoExtent =
          computeGizmoExtent(lightPosition,
                             std::clamp(anchor.worldRadius * 0.015f, 0.04f,
                                        0.75f));

      gizmoPushConstants.positionRadius = glm::vec4(
          lightPosition, pointGizmoExtent);
      gizmoPushConstants.colorIntensity = glm::vec4(lightColor, 1.0f);
      vkCmdPushConstants(commandBuffer, lightingPipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(LightPushConstants), &gizmoPushConstants);
      vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }
  }

  void createLightVolumeGeometry() {
    lightVolumeIndexCount = 36;
  }

  void processInput(float deltaTime) {
    if (!camera) return;

    auto toggleFromKey = [this](int key, bool& previousState,
                                auto&& onToggle) {
      if (!window) return;
      GLFWwindow* nativeWindow = window->getNativeWindow();
      if (!nativeWindow) return;

      const bool keyDown = glfwGetKey(nativeWindow, key) == GLFW_PRESS;
      if (keyDown && !previousState) {
        onToggle();
      }
      previousState = keyDown;
    };

    toggleFromKey(GLFW_KEY_F6, debugDirectionalOnlyKeyDown, [this]() {
      debugDirectionalOnly = !debugDirectionalOnly;
      if (guiManager) {
        guiManager->setStatusMessage(debugDirectionalOnly
                                         ? "Debug: directional-only enabled"
                                         : "Debug: directional-only disabled");
      }
    });

    toggleFromKey(GLFW_KEY_F7, debugVisualizePointLightStencilKeyDown,
                  [this]() {
                    debugVisualizePointLightStencil =
                        !debugVisualizePointLightStencil;
                    if (guiManager) {
                      guiManager->setStatusMessage(
                          debugVisualizePointLightStencil
                              ? "Debug: point-light stencil visualization enabled"
                              : "Debug: point-light stencil visualization disabled");
                    }
                  });

    toggleFromKey(GLFW_KEY_F8, debugFlipPointLightFrontFaceKeyDown, [this]() {
      debugFlipPointLightFrontFace = !debugFlipPointLightFrontFace;
      if (guiManager) {
        guiManager->setStatusMessage(
            debugFlipPointLightFrontFace
                ? "Debug: flipped front-face for stencil volume pass"
                : "Debug: default front-face for stencil volume pass");
      }
    });

    if (guiManager && guiManager->isCapturingInput() &&
        !inputManager.isLooking()) {
      return;
    }

    const bool cameraChanged = inputManager.update(deltaTime);
    if (cameraChanged) {
      updateCameraBuffer();
    }
  }

  void writeToBuffer(const utility::memory::AllocatedBuffer& buffer,
                     const void* data, size_t size) {
    if (!allocationManager) return;
    void* mapped = buffer.allocation_info.pMappedData;
    bool mappedHere = false;
    if (mapped == nullptr) {
      if (vmaMapMemory(allocationManager->memoryManager()->allocator(),
                       buffer.allocation, &mapped) != VK_SUCCESS) {
        throw std::runtime_error("failed to map buffer for writing");
      }
      mappedHere = true;
    }

    std::memcpy(mapped, data, size);

    if (mappedHere) {
      vmaUnmapMemory(allocationManager->memoryManager()->allocator(),
                     buffer.allocation);
    }
  }

  void updateCameraBuffer() {
    if (!swapChainManager || !camera) return;
    const auto extent = swapChainManager->extent();
    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(extent.height);

    cameraData.viewProj = toShaderMatrix(camera->viewProjection(aspect));
    cameraData.inverseViewProj = glm::inverse(cameraData.viewProj);
    cameraData.cameraWorldPosition = glm::vec4(camera->position(), 1.0f);
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      writeToBuffer(cameraBuffer, &cameraData, sizeof(CameraData));
    }
  }

  void syncObjectDataFromSceneGraph() {
    sceneGraph.updateWorldTransforms();
    renderableNodes.assign(sceneGraph.renderableNodes().begin(),
                           sceneGraph.renderableNodes().end());
    objectData.clear();
    opaqueDrawCommands.clear();
    transparentDrawCommands.clear();
    objectData.reserve(renderableNodes.size());
    opaqueDrawCommands.reserve(renderableNodes.size());
    transparentDrawCommands.reserve(renderableNodes.size());

    for (const uint32_t nodeIndex : renderableNodes) {
      const auto* node = sceneGraph.getNode(nodeIndex);
      if (!node ||
          node->primitiveIndex == utility::scene::SceneGraph::kInvalidNode ||
          node->primitiveIndex >= sceneManager->primitiveRanges().size()) {
        continue;
      }

      const auto& primitive =
          sceneManager->primitiveRanges()[node->primitiveIndex];
      const uint32_t materialIndex = node->materialIndex;

      ObjectData object{};
      object.model = toShaderMatrix(node->worldTransform);
      object.normalMatrix = toShaderNormalMatrix(node->worldTransform);
      object.color = sceneManager->resolveMaterialColor(materialIndex);
      object.emissiveColor =
          sceneManager->resolveMaterialEmissive(materialIndex);
      object.metallicRoughness =
          sceneManager->resolveMaterialMetallicRoughnessFactors(materialIndex);
      object.baseColorTextureIndex =
          sceneManager->resolveMaterialTextureIndex(materialIndex);
      object.normalTextureIndex =
          sceneManager->resolveMaterialNormalTexture(materialIndex);
      object.occlusionTextureIndex =
          sceneManager->resolveMaterialOcclusionTexture(materialIndex);
      object.emissiveTextureIndex =
          sceneManager->resolveMaterialEmissiveTexture(materialIndex);
      object.metallicRoughnessTextureIndex =
          sceneManager->resolveMaterialMetallicRoughnessTexture(materialIndex);
      object.alphaCutoff =
          sceneManager->resolveMaterialAlphaCutoff(materialIndex);
      object.flags = 0;
      if (sceneManager->isMaterialAlphaMasked(materialIndex)) {
        object.flags |= kObjectFlagAlphaMask;
      }
      if (sceneManager->isMaterialTransparent(materialIndex)) {
        object.flags |= kObjectFlagAlphaBlend;
      }
      if (sceneManager->isMaterialDoubleSided(materialIndex)) {
        object.flags |= kObjectFlagDoubleSided;
      }

      const uint32_t objectIndex = static_cast<uint32_t>(objectData.size());
      objectData.push_back(object);

      DrawCommand drawCommand{};
      drawCommand.objectIndex = objectIndex;
      drawCommand.firstIndex = primitive.firstIndex;
      drawCommand.indexCount = primitive.indexCount;

      if ((object.flags & kObjectFlagAlphaBlend) != 0u) {
        transparentDrawCommands.push_back(drawCommand);
      } else {
        opaqueDrawCommands.push_back(drawCommand);
      }
    }

    // Diagnostic cube: appended last so its objectIndex is always known.
    diagCubeObjectIndex = std::numeric_limits<uint32_t>::max();
    if (guiManager && guiManager->showNormalDiagCube() &&
        diagCubeVertexSlice.buffer != VK_NULL_HANDLE) {
      ObjectData cubeObject{}; // defaults: all textures=invalid
      glm::vec3 diagCenter{0.0f};
      float diagScale = 0.5f;
      if (sceneManager) {
        const auto& bounds = sceneManager->modelBounds();
        if (bounds.valid) {
          diagCenter = bounds.center + glm::vec3(bounds.radius * 1.75f, 0.0f, 0.0f);
          diagScale = std::max(0.25f, bounds.radius * 0.45f);
        }
      }
      const glm::mat4 cubeModel =
          glm::translate(glm::mat4(1.0f), diagCenter) *
          glm::scale(glm::mat4(1.0f), glm::vec3(diagScale));
      cubeObject.model = toShaderMatrix(cubeModel);
      cubeObject.normalMatrix = toShaderNormalMatrix(cubeModel);
      cubeObject.color = glm::vec4(1.0f);
      cubeObject.metallicRoughness = glm::vec2(0.0f, 0.5f);
      diagCubeObjectIndex = static_cast<uint32_t>(objectData.size());
      objectData.push_back(cubeObject);
    }
  }

  void updateObjectBuffer() {
    syncObjectDataFromSceneGraph();
    const bool bufferRecreated = ensureObjectBufferCapacity(objectData.size());
    if (bufferRecreated) {
      sceneManager->updateDescriptorSet(cameraBuffer, objectBuffer);
    }
    if (objectBuffer.buffer == VK_NULL_HANDLE) return;
    if (objectData.empty()) return;
    writeToBuffer(objectBuffer, objectData.data(),
                  sizeof(ObjectData) * objectData.size());
  }

  bool reloadSceneModel(const std::string& path) {
    if (!sceneManager) return false;
    vkDeviceWaitIdle(deviceWrapper->device());
    const bool result =
        sceneManager->reloadModel(path, cameraBuffer, objectBuffer);
    indexType = sceneManager->indexType();
    buildSceneGraph();
    ensureObjectBufferCapacity(sceneGraph.renderableNodes().size());
    createGeometryBuffers();
    updateLightingData();
    createLightVolumeGeometry();
    if (result) {
      config_.modelPath = path;
      resetCameraForScene();
      updateCameraBuffer();
    }
    updateObjectBuffer();
    sceneManager->updateDescriptorSet(cameraBuffer, objectBuffer);
    if (guiManager) {
      guiManager->setStatusMessage(result ? "Loaded model: " + path
                                          : "Failed to load model: " + path);
    }
    return result;
  }

  void createGeometryBuffers() {
    const auto& sceneVertices = sceneManager->vertices();
    const auto& sceneIndices = sceneManager->indices();
    const geometry::Model cube = geometry::Model::MakeCube();

    std::vector<geometry::Vertex> mergedVertices;
    mergedVertices.reserve(sceneVertices.size() + cube.vertices().size());
    mergedVertices.insert(mergedVertices.end(), sceneVertices.begin(),
                          sceneVertices.end());
    const uint32_t diagVertexBase = static_cast<uint32_t>(mergedVertices.size());
    mergedVertices.insert(mergedVertices.end(), cube.vertices().begin(),
                          cube.vertices().end());

    std::vector<uint32_t> mergedIndices;
    mergedIndices.reserve(sceneIndices.size() + cube.indices().size());
    mergedIndices.insert(mergedIndices.end(), sceneIndices.begin(),
                         sceneIndices.end());
    const VkDeviceSize diagIndexByteOffset =
        static_cast<VkDeviceSize>(mergedIndices.size()) * sizeof(uint32_t);
    for (const uint32_t index : cube.indices()) {
      mergedIndices.push_back(index + diagVertexBase);
    }

    vertexSlice = allocationManager->uploadVertices(
        std::span<const geometry::Vertex>(mergedVertices));
    indexSlice = allocationManager->uploadIndices(
        std::span<const uint32_t>(mergedIndices));

    diagCubeVertexSlice = vertexSlice;
    diagCubeIndexSlice = indexSlice;
    diagCubeIndexSlice.offset = indexSlice.offset + diagIndexByteOffset;
    diagCubeIndexSlice.size =
        static_cast<VkDeviceSize>(cube.indices().size()) * sizeof(uint32_t);
    diagCubeIndexCount = static_cast<uint32_t>(cube.indices().size());
  }

  void createCommandBuffers() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount =
        static_cast<uint32_t>(swapChainManager->imageCount());

    commandBuffers.resize(swapChainManager->imageCount());
    if (vkAllocateCommandBuffers(deviceWrapper->device(), &allocInfo,
                                 commandBuffers.data()) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate command buffers!");
    }
  }

  void recreateCommandBuffers() {
    if (!commandBuffers.empty()) {
      vkFreeCommandBuffers(deviceWrapper->device(), commandPool,
                           static_cast<uint32_t>(commandBuffers.size()),
                           commandBuffers.data());
      commandBuffers.clear();
    }
    createCommandBuffers();
  }

  void setViewportAndScissor(VkCommandBuffer commandBuffer) const {
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapChainManager->extent().width);
    viewport.height = static_cast<float>(swapChainManager->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapChainManager->extent();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
  }

  void bindSceneGeometryBuffers(VkCommandBuffer commandBuffer) const {
    if (vertexSlice.buffer == VK_NULL_HANDLE ||
        indexSlice.buffer == VK_NULL_HANDLE) {
      return;
    }
    VkBuffer vertexBuffers[] = {vertexSlice.buffer};
    VkDeviceSize offsets[] = {vertexSlice.offset};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexSlice.buffer, indexSlice.offset,
                         indexType);
  }

  void drawSceneGeometry(VkCommandBuffer commandBuffer,
                         const std::vector<DrawCommand>& drawCommands,
                         VkPipelineLayout pipelineLayout) {
    for (const DrawCommand& drawCommand : drawCommands) {
      pushConstants.objectIndex = drawCommand.objectIndex;
      vkCmdPushConstants(
          commandBuffer, pipelineLayout,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
          sizeof(BindlessPushConstants), &pushConstants);
      vkCmdDrawIndexed(commandBuffer, drawCommand.indexCount, 1,
                       drawCommand.firstIndex, 0, 0);
    }
  }

  void clearExactOitResources(VkCommandBuffer commandBuffer,
                              const FrameResources& frame) const {
    VkImageMemoryBarrier headPointerClearBarrier{};
    headPointerClearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    headPointerClearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    headPointerClearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    headPointerClearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    headPointerClearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    headPointerClearBarrier.image = frame.oitHeadPointers.image;
    headPointerClearBarrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    headPointerClearBarrier.subresourceRange.baseMipLevel = 0;
    headPointerClearBarrier.subresourceRange.levelCount = 1;
    headPointerClearBarrier.subresourceRange.baseArrayLayer = 0;
    headPointerClearBarrier.subresourceRange.layerCount = 1;
    headPointerClearBarrier.srcAccessMask = 0;
    headPointerClearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkBufferMemoryBarrier counterClearBarrier{};
    counterClearBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    counterClearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counterClearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counterClearBarrier.buffer = frame.oitCounterBuffer.buffer;
    counterClearBarrier.offset = 0;
    counterClearBarrier.size = sizeof(uint32_t);
    counterClearBarrier.srcAccessMask = 0;
    counterClearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                         &counterClearBarrier, 1, &headPointerClearBarrier);

    VkClearColorValue clearColor{};
    clearColor.uint32[0] = kInvalidOitNodeIndex;
    const VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                             0, 1};
    vkCmdClearColorImage(commandBuffer, frame.oitHeadPointers.image,
                         VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);
    vkCmdFillBuffer(commandBuffer, frame.oitCounterBuffer.buffer, 0,
                    sizeof(uint32_t), 0u);

    VkImageMemoryBarrier headPointerReadyBarrier = headPointerClearBarrier;
    headPointerReadyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    headPointerReadyBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    VkBufferMemoryBarrier counterReadyBarrier = counterClearBarrier;
    counterReadyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    counterReadyBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         1, &counterReadyBarrier, 1, &headPointerReadyBarrier);
  }

  void prepareExactOitResolve(VkCommandBuffer commandBuffer,
                              const FrameResources& frame) const {
    VkImageMemoryBarrier headPointerResolveBarrier{};
    headPointerResolveBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    headPointerResolveBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    headPointerResolveBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    headPointerResolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    headPointerResolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    headPointerResolveBarrier.image = frame.oitHeadPointers.image;
    headPointerResolveBarrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    headPointerResolveBarrier.subresourceRange.baseMipLevel = 0;
    headPointerResolveBarrier.subresourceRange.levelCount = 1;
    headPointerResolveBarrier.subresourceRange.baseArrayLayer = 0;
    headPointerResolveBarrier.subresourceRange.layerCount = 1;
    headPointerResolveBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    headPointerResolveBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    VkBufferMemoryBarrier nodeResolveBarrier{};
    nodeResolveBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    nodeResolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    nodeResolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    nodeResolveBarrier.buffer = frame.oitNodeBuffer.buffer;
    nodeResolveBarrier.offset = 0;
    nodeResolveBarrier.size = VK_WHOLE_SIZE;
    nodeResolveBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    nodeResolveBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    VkBufferMemoryBarrier counterResolveBarrier{};
    counterResolveBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    counterResolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counterResolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counterResolveBarrier.buffer = frame.oitCounterBuffer.buffer;
    counterResolveBarrier.offset = 0;
    counterResolveBarrier.size = sizeof(uint32_t);
    counterResolveBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    counterResolveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    std::array<VkBufferMemoryBarrier, 2> bufferBarriers = {
        nodeResolveBarrier, counterResolveBarrier};
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         static_cast<uint32_t>(bufferBarriers.size()),
                         bufferBarriers.data(), 1, &headPointerResolveBarrier);
  }

  void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      throw std::runtime_error("failed to begin recording command buffer!");
    }

    if (imageIndex >= frameResources.size()) {
      throw std::runtime_error("frame resource index out of range");
    }

    VkDescriptorSet sceneDescriptorSet = sceneManager->descriptorSet();
    const auto& frame = frameResources[imageIndex];
    const std::array<VkDescriptorSet, 2> lightingDescriptorSets = {
        frame.lightingDescriptorSet, lightDescriptorSet};
    const std::array<VkDescriptorSet, 3> transparentDescriptorSets = {
        sceneDescriptorSet, lightDescriptorSet, frame.oitDescriptorSet};
    const std::array<VkDescriptorSet, 2> postProcessDescriptorSets = {
        frame.postProcessDescriptorSet, frame.oitDescriptorSet};

    VkRenderPassBeginInfo depthPrepassInfo{};
    depthPrepassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    depthPrepassInfo.renderPass = depthPrepassRenderPass;
    depthPrepassInfo.framebuffer = frame.depthPrepassFramebuffer;
    depthPrepassInfo.renderArea.offset = {0, 0};
    depthPrepassInfo.renderArea.extent = swapChainManager->extent();

    VkClearValue depthPrepassClearValue{};
    depthPrepassClearValue.depthStencil = {0.0f, 0};
    depthPrepassInfo.clearValueCount = 1;
    depthPrepassInfo.pClearValues = &depthPrepassClearValue;

    vkCmdBeginRenderPass(commandBuffer, &depthPrepassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      depthPrepassPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scenePipelineLayout, 0, 1, &sceneDescriptorSet, 0,
                            nullptr);
    setViewportAndScissor(commandBuffer);
    bindSceneGeometryBuffers(commandBuffer);
    drawSceneGeometry(commandBuffer, opaqueDrawCommands, scenePipelineLayout);
    if (diagCubeObjectIndex != std::numeric_limits<uint32_t>::max() &&
        diagCubeVertexSlice.buffer != VK_NULL_HANDLE) {
      VkBuffer diagVB[] = {diagCubeVertexSlice.buffer};
      VkDeviceSize diagOff[] = {diagCubeVertexSlice.offset};
      vkCmdBindVertexBuffers(commandBuffer, 0, 1, diagVB, diagOff);
      vkCmdBindIndexBuffer(commandBuffer, diagCubeIndexSlice.buffer,
                           diagCubeIndexSlice.offset, VK_INDEX_TYPE_UINT32);
      pushConstants.objectIndex = diagCubeObjectIndex;
      vkCmdPushConstants(commandBuffer, scenePipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(BindlessPushConstants), &pushConstants);
      vkCmdDrawIndexed(commandBuffer, diagCubeIndexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(commandBuffer);

    VkRenderPassBeginInfo gBufferPassInfo{};
    gBufferPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    gBufferPassInfo.renderPass = gBufferRenderPass;
    gBufferPassInfo.framebuffer = frame.gBufferFramebuffer;
    gBufferPassInfo.renderArea.offset = {0, 0};
    gBufferPassInfo.renderArea.extent = swapChainManager->extent();

    std::array<VkClearValue, 6> gBufferClearValues{};
    gBufferClearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    gBufferClearValues[1].color = {{0.5f, 0.5f, 1.0f, 1.0f}};
    gBufferClearValues[2].color = {{0.0f, 1.0f, 1.0f, 1.0f}};
    gBufferClearValues[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    gBufferClearValues[4].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    gBufferClearValues[5].depthStencil = {0.0f, 0};
    gBufferPassInfo.clearValueCount =
        static_cast<uint32_t>(gBufferClearValues.size());
    gBufferPassInfo.pClearValues = gBufferClearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &gBufferPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      gBufferPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scenePipelineLayout, 0, 1, &sceneDescriptorSet, 0,
                            nullptr);
    setViewportAndScissor(commandBuffer);
    bindSceneGeometryBuffers(commandBuffer);
    drawSceneGeometry(commandBuffer, opaqueDrawCommands, scenePipelineLayout);
    if (diagCubeObjectIndex != std::numeric_limits<uint32_t>::max() &&
        diagCubeVertexSlice.buffer != VK_NULL_HANDLE) {
      VkBuffer diagVB[] = {diagCubeVertexSlice.buffer};
      VkDeviceSize diagOff[] = {diagCubeVertexSlice.offset};
      vkCmdBindVertexBuffers(commandBuffer, 0, 1, diagVB, diagOff);
      vkCmdBindIndexBuffer(commandBuffer, diagCubeIndexSlice.buffer,
                           diagCubeIndexSlice.offset, VK_INDEX_TYPE_UINT32);
      pushConstants.objectIndex = diagCubeObjectIndex;
      vkCmdPushConstants(commandBuffer, scenePipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(BindlessPushConstants), &pushConstants);
      vkCmdDrawIndexed(commandBuffer, diagCubeIndexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(commandBuffer);

    clearExactOitResources(commandBuffer, frame);

    VkRenderPassBeginInfo lightingPassInfo{};
    lightingPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    lightingPassInfo.renderPass = lightingRenderPass;
    lightingPassInfo.framebuffer = frame.lightingFramebuffer;
    lightingPassInfo.renderArea.offset = {0, 0};
    lightingPassInfo.renderArea.extent = swapChainManager->extent();

    std::array<VkClearValue, 2> lightingClearValues{};
    lightingClearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    lightingClearValues[1].depthStencil = {0.0f, 0};
    lightingPassInfo.clearValueCount =
        static_cast<uint32_t>(lightingClearValues.size());
    lightingPassInfo.pClearValues = lightingClearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &lightingPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      directionalLightPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            lightingPipelineLayout, 0,
                            static_cast<uint32_t>(lightingDescriptorSets.size()),
                            lightingDescriptorSets.data(), 0, nullptr);
    setViewportAndScissor(commandBuffer);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    VkClearAttachment stencilClearAttachment{};
    stencilClearAttachment.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    stencilClearAttachment.clearValue.depthStencil = {0.0f, 0};
    VkClearRect stencilClearRect{};
    stencilClearRect.rect.offset = {0, 0};
    stencilClearRect.rect.extent = swapChainManager->extent();
    stencilClearRect.baseArrayLayer = 0;
    stencilClearRect.layerCount = 1;

    if (!debugDirectionalOnly) {
      const VkPipeline activeStencilPipeline =
          debugFlipPointLightFrontFace ? stencilVolumePipelineFlipped
                                       : stencilVolumePipeline;
      const VkPipeline activePointPipeline =
          debugVisualizePointLightStencil ? pointLightStencilDebugPipeline
                                          : pointLightPipeline;
      for (uint32_t i = 0;
           i < std::min(lightingData.pointLightCount, kMaxDeferredPointLights);
           ++i) {
        vkCmdClearAttachments(commandBuffer, 1, &stencilClearAttachment, 1,
                              &stencilClearRect);
        lightPushConstants.positionRadius =
            lightingData.pointLights[i].positionRadius;
        lightPushConstants.colorIntensity =
            lightingData.pointLights[i].colorIntensity;

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          activeStencilPipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            lightingPipelineLayout, 0,
            static_cast<uint32_t>(lightingDescriptorSets.size()),
            lightingDescriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(commandBuffer, lightingPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), &lightPushConstants);
        vkCmdDraw(commandBuffer, lightVolumeIndexCount, 1, 0, 0);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          activePointPipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            lightingPipelineLayout, 0,
            static_cast<uint32_t>(lightingDescriptorSets.size()),
            lightingDescriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(commandBuffer, lightingPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), &lightPushConstants);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
      }
    }

    if (!transparentDrawCommands.empty()) {
      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentPipeline);
      vkCmdBindDescriptorSets(
          commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
          transparentPipelineLayout, 0,
          static_cast<uint32_t>(transparentDescriptorSets.size()),
          transparentDescriptorSets.data(), 0, nullptr);
      bindSceneGeometryBuffers(commandBuffer);
      drawSceneGeometry(commandBuffer, transparentDrawCommands,
                        transparentPipelineLayout);
    }

    if (guiManager && guiManager->showGeometryOverlay() &&
        geometryDebugPipeline != VK_NULL_HANDLE) {
      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        geometryDebugPipeline);
      vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scenePipelineLayout, 0, 1, &sceneDescriptorSet, 0,
                              nullptr);
      bindSceneGeometryBuffers(commandBuffer);
      drawSceneGeometry(commandBuffer, opaqueDrawCommands, scenePipelineLayout);
      drawSceneGeometry(commandBuffer, transparentDrawCommands,
                        scenePipelineLayout);
    }

    if (guiManager &&
        guiManager->gBufferViewMode() ==
            utility::ui::GBufferViewMode::SurfaceNormals &&
        surfaceNormalLinePipeline != VK_NULL_HANDLE) {
      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        surfaceNormalLinePipeline);
      vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scenePipelineLayout, 0, 1, &sceneDescriptorSet, 0,
                              nullptr);
      bindSceneGeometryBuffers(commandBuffer);
      drawSceneGeometry(commandBuffer, opaqueDrawCommands, scenePipelineLayout);
      drawSceneGeometry(commandBuffer, transparentDrawCommands,
                        scenePipelineLayout);
    }

    if (guiManager && guiManager->showLightGizmos() &&
        lightGizmoPipeline != VK_NULL_HANDLE) {
      drawLightGizmos(commandBuffer, lightingDescriptorSets);
    }

    vkCmdEndRenderPass(commandBuffer);
    prepareExactOitResolve(commandBuffer, frame);

    VkRenderPassBeginInfo postProcessPassInfo{};
    postProcessPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    postProcessPassInfo.renderPass = postProcessRenderPass;
    postProcessPassInfo.framebuffer =
        swapChainManager->framebuffers()[imageIndex];
    postProcessPassInfo.renderArea.offset = {0, 0};
    postProcessPassInfo.renderArea.extent = swapChainManager->extent();

    VkClearValue postProcessClearValue{};
    postProcessClearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    postProcessPassInfo.clearValueCount = 1;
    postProcessPassInfo.pClearValues = &postProcessClearValue;

    vkCmdBeginRenderPass(commandBuffer, &postProcessPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      postProcessPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            postProcessPipelineLayout, 0,
                            static_cast<uint32_t>(postProcessDescriptorSets.size()),
                            postProcessDescriptorSets.data(), 0, nullptr);
    setViewportAndScissor(commandBuffer);

    PostProcessPushConstants postProcessPushConstants{};
    postProcessPushConstants.outputMode = static_cast<uint32_t>(
        guiManager ? guiManager->gBufferViewMode()
                   : utility::ui::GBufferViewMode::Overview);
    vkCmdPushConstants(commandBuffer, postProcessPipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PostProcessPushConstants),
                       &postProcessPushConstants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    if (guiManager) {
      guiManager->render(commandBuffer);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to record command buffer!");
    }
  }

  void drawFrame() {
    frameSyncManager->waitForFrame(currentFrame);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        deviceWrapper->device(), swapChainManager->swapChain(), UINT64_MAX,
        frameSyncManager->imageAvailable(currentFrame), VK_NULL_HANDLE,
        &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      recreateSwapChain();
      return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      throw std::runtime_error("failed to acquire swap chain image!");
    }

    if (imagesInFlight[imageIndex]) {
      VkFence inFlightFence = imagesInFlight[imageIndex];
      vkWaitForFences(deviceWrapper->device(), 1, &inFlightFence, VK_TRUE,
                      UINT64_MAX);
    }

    growExactOitNodePoolIfNeeded(imageIndex);

    frameSyncManager->resetFence(currentFrame);
    imagesInFlight[imageIndex] = frameSyncManager->fence(currentFrame);

    updateObjectBuffer();
    if (guiManager) {
      guiManager->startFrame();
      guiManager->drawSceneControls(
          sceneGraph,
          [this](const std::string& modelPath) {
            return reloadSceneModel(modelPath);
          },
          [this]() {
            return reloadSceneModel(app::DefaultAppConfig().modelPath);
          },
          cameraTransformControls(),
          [this](const utility::ui::TransformControls& controls) {
            applyCameraTransform(controls);
          },
          nodeTransformControls(rootNode),
          [this](const utility::ui::TransformControls& controls) {
            applyNodeTransform(rootNode, controls);
          },
          directionalLightPosition(),
          lightingData,
          selectedMeshNode,
          [this](uint32_t nodeIndex) { selectMeshNode(nodeIndex); },
          nodeTransformControls(selectedMeshNode),
          [this](uint32_t nodeIndex,
                 const utility::ui::TransformControls& controls) {
            applyNodeTransform(nodeIndex, controls);
          });
    }

    vkResetCommandBuffer(commandBuffers[imageIndex], 0);
    recordCommandBuffer(commandBuffers[imageIndex], imageIndex);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {
        frameSyncManager->imageAvailable(currentFrame)};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    VkCommandBuffer commandBufferHandle = commandBuffers[imageIndex];
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBufferHandle;

    VkSemaphore signalSemaphores[] = {
        frameSyncManager->renderFinishedForImage(imageIndex)};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(deviceWrapper->graphicsQueue(), 1, &submitInfo,
                      frameSyncManager->fence(currentFrame)) != VK_SUCCESS) {
      throw std::runtime_error("failed to submit draw command buffer!");
    }

    result = swapChainManager->present(
        deviceWrapper->presentQueue(), imageIndex,
        frameSyncManager->renderFinishedForImage(imageIndex));

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        framebufferResized) {
      framebufferResized = false;
      recreateSwapChain();
    } else if (result != VK_SUCCESS) {
      throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % config_.maxFramesInFlight;
  }
};




int main(int argc, char** argv) {
  try {
    auto config = app::DefaultAppConfig();
    if (argc > 1 && argv[1] != nullptr && std::strlen(argv[1]) > 0) {
      config.modelPath = argv[1];
    }

    HelloTriangleApplication app{std::move(config)};
    app.run();
  } catch (const std::exception& e) {
    std::println(stderr, "{}", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
