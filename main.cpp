
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
#include <boost/core/span.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>


using utility::FrameSyncManager;
using utility::QueueFamilyIndices;
using utility::SwapChainManager;
using utility::SwapChainSupportDetails;
namespace window = utility::window;


class HelloTriangleApplication {
 public:
  explicit HelloTriangleApplication(app::AppConfig config)
      : config_(std::move(config)) {
    gltfPathInput = config_.modelPath;
  }

  void run() {
    initWindow();
    initVulkan();
    mainLoop();
    cleanup();
  }

 private:
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

  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  VkPipeline graphicsPipeline{VK_NULL_HANDLE};
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

  utility::memory::AllocatedBuffer cameraBuffer{};
  utility::memory::AllocatedBuffer objectBuffer{};

  CameraData cameraData{};
  std::vector<ObjectData> objectData;
  BindlessPushConstants pushConstants{};

  std::unique_ptr<utility::camera::BaseCamera> camera;
  utility::input::InputManager inputManager{};
  double lastFrameTimeSeconds{0.0};

  utility::memory::BufferSlice vertexSlice{};
  utility::memory::BufferSlice indexSlice{};

  std::unique_ptr<FrameSyncManager> frameSyncManager;
  std::vector<VkFence> imagesInFlight;
  uint32_t currentFrame = 0;

  bool framebufferResized = false;

  void initWindow() {
    windowManager = std::make_unique<window::WindowManager>();
    window = windowManager->createWindow(config_.windowWidth,
                                         config_.windowHeight, "Vulkan");
    inputManager.bindWindow(window->getNativeWindow());
    window->setFramebufferResizeCallback(
        [this](int, int) { framebufferResized = true; });
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
    createRenderPass();
    createCommandPool();
    allocationManager = std::make_unique<utility::memory::AllocationManager>();
    allocationManager->initialize(
        instance, deviceWrapper->physicalDevice(), deviceWrapper->device(),
        deviceWrapper->graphicsQueue(), commandPool.get(), config_);
    sceneManager = std::make_unique<utility::scene::SceneManager>(
        *allocationManager, *pipelineManager, deviceWrapper, config_);
    sceneManager->initialize(config_.modelPath);
    indexType = sceneManager->indexType();
    createGraphicsPipeline();
    swapChainManager->createFramebuffers(renderPass.get());
    createCamera();
    createSceneBuffers();
    createVertexBuffer();
    createIndexBuffer();
    buildSceneGraph();
    sceneManager->updateDescriptorSet(cameraBuffer, objectBuffer);
    guiManager = std::make_unique<utility::ui::GuiManager>();
    guiManager->initialize(
        instance, deviceWrapper->device(), deviceWrapper->physicalDevice(),
        deviceWrapper->graphicsQueue(),
        deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
        renderPass.get(), swapChainManager->imageCount(),
        window->getNativeWindow(), config_.modelPath);
    createCommandBuffers();
    frameSyncManager = std::make_unique<FrameSyncManager>(
        deviceWrapper->device(), config_.maxFramesInFlight);
    frameSyncManager->initialize(swapChainManager->imageCount());
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

    if (guiManager) {
      guiManager->shutdown(deviceWrapper->device());
      guiManager.reset();
    }

    if (renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(deviceWrapper->device(), renderPass, nullptr);
      renderPass = VK_NULL_HANDLE;
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
      allocationManager->cleanup();
      allocationManager.reset();
    }

    if (pipelineManager) {
      pipelineManager->destroyManagedResources();
      graphicsPipeline = VK_NULL_HANDLE;
      pipelineLayout = VK_NULL_HANDLE;
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

    instance.destroySurfaceKHR(surface);
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
      swapChainManager->recreate(renderPass.get());
      recreateCommandBuffers();
      if (guiManager) {
        guiManager->updateSwapchainImageCount(swapChainManager->imageCount());
      }
      frameSyncManager->recreateRenderFinishedSemaphores(
          swapChainManager->imageCount());
      imagesInFlight.assign(swapChainManager->imageCount(), VK_NULL_HANDLE);
      updateCameraBuffer();
      updateObjectBuffer();
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
    createInfo.sType =
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
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
    descriptorIndexingFeatures.pNext = &bufferDeviceAddressFeatures;
    createInfo.next = &descriptorIndexingFeatures;

    deviceWrapper = std::make_shared<utility::vulkan::VulkanDevice>(
        instance, surface, createInfo);
  }

  void createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapChainManager->imageFormat();
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(deviceWrapper->device(), &renderPassInfo, nullptr,
                           &renderPass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create render pass!");
    }
  }

  void buildSceneGraph() {
    sceneGraph = utility::scene::SceneGraph{};
    renderableNodes.clear();
    rootNode = sceneGraph.createNode(
        glm::mat4(1.0f), sceneManager->defaultMaterialIndex(), false);
    cubeNode = sceneGraph.createNode(
        glm::mat4(1.0f), sceneManager->defaultMaterialIndex(), true);
    sceneGraph.setParent(cubeNode, rootNode);
    sceneGraph.updateWorldTransforms();
    renderableNodes = sceneGraph.renderableNodes();
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

  void createGraphicsPipeline() {
    const auto vertShaderCode =
        utility::file::readFile("spv_shaders/base.vert.spv");
    const auto fragShaderCode =
        utility::file::readFile("spv_shaders/base.frag.spv");

    VkShaderModule vertShaderModule = utility::vulkan::createShaderModule(
        deviceWrapper->device(), vertShaderCode);
    VkShaderModule fragShaderModule = utility::vulkan::createShaderModule(
        deviceWrapper->device(), fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo, fragShaderStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    auto bindingDescription = geometry::Vertex::bindingDescription();
    auto attributeDescriptions = geometry::Vertex::attributeDescriptions();

    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.pVertexAttributeDescriptions =
        attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(BindlessPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = {
        sceneManager->descriptorSetLayout()};
    std::vector<VkPushConstantRange> pushConstants = {pushConstantRange};
    pipelineLayout =
        pipelineManager->createPipelineLayout(setLayouts, pushConstants);

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    graphicsPipeline = pipelineManager->createGraphicsPipeline(
        pipelineInfo, "base_bindless_pipeline");

    vkDestroyShaderModule(deviceWrapper->device(), fragShaderModule, nullptr);
    vkDestroyShaderModule(deviceWrapper->device(), vertShaderModule, nullptr);
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

  void createSceneBuffers() {
    cameraBuffer = allocationManager->createBuffer(
        sizeof(CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    objectBuffer = allocationManager->createBuffer(
        sizeof(ObjectData) * config_.maxSceneObjects,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    updateCameraBuffer();
    updateObjectBuffer();
  }

  void createCamera() {
    auto perspective = std::make_unique<utility::camera::PerspectiveCamera>();
    perspective->setPosition({2.0f, 2.0f, 2.0f});
    perspective->setYawPitch(-135.0f, -35.0f);
    camera = std::move(perspective);
    inputManager.setCamera(camera.get());
    inputManager.setMoveSpeed(3.5f);
    inputManager.setMouseSensitivity(0.15f);
  }

  void processInput(float deltaTime) {
    if (!camera) return;

    if (guiManager && guiManager->isCapturingInput()) return;

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

    cameraData.viewProj = camera->viewProjection(aspect);
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      writeToBuffer(cameraBuffer, &cameraData, sizeof(CameraData));
    }
  }

  void syncObjectDataFromSceneGraph() {
    sceneGraph.updateWorldTransforms();
    renderableNodes = sceneGraph.renderableNodes();
    const size_t objectCount =
        std::min<size_t>(renderableNodes.size(), config_.maxSceneObjects);
    objectData.resize(objectCount);

    for (size_t i = 0; i < objectCount; ++i) {
      const uint32_t nodeIndex = renderableNodes[i];
      const auto* node = sceneGraph.getNode(nodeIndex);
      const glm::mat4 model = node ? node->worldTransform : glm::mat4(1.0f);
      const uint32_t materialIndex =
          node ? node->materialIndex : sceneManager->defaultMaterialIndex();
      objectData[i].model = model;
      objectData[i].color = sceneManager->resolveMaterialColor(materialIndex);
      objectData[i].emissiveColor =
          sceneManager->resolveMaterialEmissive(materialIndex);
      objectData[i].metallicRoughness =
          sceneManager->resolveMaterialMetallicRoughnessFactors(materialIndex);
      objectData[i].baseColorTextureIndex =
          sceneManager->resolveMaterialTextureIndex(materialIndex);
      objectData[i].normalTextureIndex =
          sceneManager->resolveMaterialNormalTexture(materialIndex);
      objectData[i].occlusionTextureIndex =
          sceneManager->resolveMaterialOcclusionTexture(materialIndex);
      objectData[i].emissiveTextureIndex =
          sceneManager->resolveMaterialEmissiveTexture(materialIndex);
      objectData[i].metallicRoughnessTextureIndex =
          sceneManager->resolveMaterialMetallicRoughnessTexture(materialIndex);
    }
  }

  void updateObjectBuffer() {
    if (objectBuffer.buffer == VK_NULL_HANDLE) return;
    syncObjectDataFromSceneGraph();
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
    if (result) {
      config_.modelPath = path;
    }
    createVertexBuffer();
    createIndexBuffer();
    buildSceneGraph();
    updateObjectBuffer();
    sceneManager->updateDescriptorSet(cameraBuffer, objectBuffer);
    if (guiManager) {
      guiManager->setStatusMessage(result ? "Loaded model: " + path
                                          : "Failed to load model: " + path);
    }
    return result;
  }

  void createVertexBuffer() {
    vertexSlice = allocationManager->uploadVertices(
        boost::span<const geometry::Vertex>(sceneManager->vertices()));
  }

  void createIndexBuffer() {
    indexSlice = allocationManager->uploadIndices(
        boost::span<const uint32_t>(sceneManager->indices()));
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

  void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      throw std::runtime_error("failed to begin recording command buffer!");
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = swapChainManager->framebuffers()[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChainManager->extent();

    VkClearValue clearColor{};
    clearColor.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      graphicsPipeline);
    VkDescriptorSet descriptorSet = sceneManager->descriptorSet();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

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

    VkBuffer vertexBuffers[] = {vertexSlice.buffer};
    VkDeviceSize offsets[] = {vertexSlice.offset};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindIndexBuffer(commandBuffer, indexSlice.buffer, indexSlice.offset,
                         indexType);

    const uint32_t drawCount = static_cast<uint32_t>(
        std::min<size_t>(objectData.size(), config_.maxSceneObjects));
    const std::vector<uint32_t>& indices = sceneManager->indices();
    for (uint32_t i = 0; i < drawCount; ++i) {
      pushConstants.objectIndex = i;
      vkCmdPushConstants(commandBuffer, pipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(BindlessPushConstants), &pushConstants);
      vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 1,
                       0, 0, 0);
    }

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

    frameSyncManager->resetFence(currentFrame);
    imagesInFlight[imageIndex] = frameSyncManager->fence(currentFrame);

    updateObjectBuffer();
    if (guiManager) {
      guiManager->startFrame();
      guiManager->drawSceneControls(
          sceneGraph, config_.maxSceneObjects,
          [this](const glm::mat4& transform) { addSceneObject(transform); },
          [this]() { addSceneObject(defaultTransformForNewObject()); },
          [this](const std::string& modelPath) {
            return reloadSceneModel(modelPath);
          },
          [this]() {
            return reloadSceneModel(app::DefaultAppConfig().modelPath);
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




int main() {
  try {
    HelloTriangleApplication app{app::DefaultAppConfig()};
    app.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
