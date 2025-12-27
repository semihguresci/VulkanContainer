
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
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanAlignment.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanInstance.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/WindowManager.h"
#include "Container//utility/DebugMessengerExt.h"

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

  vk::Instance instance;
  vk::DebugUtilsMessengerEXT debugMessenger;
  vk::SurfaceKHR surface;

  std::unique_ptr<SwapChainManager> swapChainManager;

  vk::UniqueRenderPass renderPass;
  vk::PipelineLayout pipelineLayout;
  vk::Pipeline graphicsPipeline;
  vk::IndexType indexType = vk::IndexType::eUint32;

  vk::UniqueCommandPool commandPool;
  std::vector<vk::UniqueCommandBuffer> commandBuffers;

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
  std::vector<vk::Fence> imagesInFlight;
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
    imagesInFlight.assign(swapChainManager->imageCount(), vk::Fence{});
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

    deviceWrapper->device().waitIdle();
  }

  void cleanup() {
    if (swapChainManager) {
      swapChainManager->cleanup();
    }

    if (guiManager) {
      guiManager->shutdown(deviceWrapper->device());
      guiManager.reset();
    }

    renderPass.reset();

    if (sceneManager) {
      sceneManager.reset();
    }

    if (allocationManager) {
      if (cameraBuffer.buffer != vk::Buffer{}) {
        allocationManager->destroyBuffer(cameraBuffer);
      }
      if (objectBuffer.buffer != vk::Buffer{}) {
        allocationManager->destroyBuffer(objectBuffer);
      }
      allocationManager->cleanup();
      allocationManager.reset();
    }

    if (pipelineManager) {
      pipelineManager->destroyManagedResources();
      graphicsPipeline = vk::Pipeline{};
      pipelineLayout = vk::PipelineLayout{};
    }

    frameSyncManager.reset();

    commandPool.reset();

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

    deviceWrapper->device().waitIdle();

    if (swapChainManager) {
      swapChainManager->recreate(renderPass.get());
      recreateCommandBuffers();
      if (guiManager) {
        guiManager->updateSwapchainImageCount(swapChainManager->imageCount());
      }
      frameSyncManager->recreateRenderFinishedSemaphores(
          swapChainManager->imageCount());
      imagesInFlight.assign(swapChainManager->imageCount(), vk::Fence{});
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

    vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (config_.enableValidationLayers) {
      populateDebugMessengerCreateInfo(debugCreateInfo);
      createInfo.next = &debugCreateInfo;
    }

    instanceWrapper =
        std::make_shared<utility::vulkan::VulkanInstance>(createInfo);
    instance = instanceWrapper->instance();
  }

  void populateDebugMessengerCreateInfo(
      vk::DebugUtilsMessengerCreateInfoEXT& createInfo) {
    createInfo = {};
    createInfo.setMessageSeverity(
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
    createInfo.setMessageType(
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance);
    createInfo.setPfnUserCallback(debugCallback);
  }

  void setupDebugMessenger() {
    if (!config_.enableValidationLayers) return;

    vk::DebugUtilsMessengerCreateInfoEXT createInfo;
    populateDebugMessengerCreateInfo(createInfo);

    if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
                                     &debugMessenger) != vk::Result::eSuccess) {
      throw std::runtime_error("failed to set up debug messenger!");
    }
  }

  void createSurface() { surface = window->createSurface(instance); }

  void createDevice() {
    utility::vulkan::DeviceCreateInfo createInfo{};
    createInfo.requiredExtensions = config_.deviceExtensions;
    createInfo.validationLayers = config_.validationLayers;
    createInfo.enableValidationLayers = config_.enableValidationLayers;

    vk::PhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
    descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount =
        VK_TRUE;
    descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing =
        VK_TRUE;

    vk::PhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
    bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;
    descriptorIndexingFeatures.pNext = &bufferDeviceAddressFeatures;
    createInfo.next = &descriptorIndexingFeatures;

    deviceWrapper = std::make_shared<utility::vulkan::VulkanDevice>(
        instance, surface, createInfo);
  }

  void createRenderPass() {
    vk::AttachmentDescription colorAttachment{};
    colorAttachment.format =
        static_cast<vk::Format>(swapChainManager->imageFormat());
    colorAttachment.samples = vk::SampleCountFlagBits::e1;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    colorAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    colorAttachment.initialLayout = vk::ImageLayout::eUndefined;
    colorAttachment.finalLayout = vk::ImageLayout::ePresentSrcKHR;

    vk::AttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = vk::ImageLayout::eColorAttachmentOptimal;

    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    vk::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.srcAccessMask = {};
    dependency.dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;

    vk::RenderPassCreateInfo renderPassInfo{};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    vk::Device device{deviceWrapper->device()};
    renderPass = device.createRenderPassUnique(renderPassInfo);
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
    auto vertShaderCode = readFile("spv_shaders/base.vert.spv");
    auto fragShaderCode = readFile("spv_shaders/base.frag.spv");

    vk::ShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    vk::ShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    vk::PipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.stage = vk::ShaderStageFlagBits::eVertex;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    vk::PipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.stage = vk::ShaderStageFlagBits::eFragment;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo, fragShaderStageInfo};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};

    auto bindingDescription = geometry::Vertex::bindingDescription();
    auto attributeDescriptions = geometry::Vertex::attributeDescriptions();

    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    vk::PipelineViewportStateCreateInfo viewportState{};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = vk::PolygonMode::eFill;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = vk::CullModeFlagBits::eBack;
    rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
    rasterizer.depthBiasEnable = VK_FALSE;

    vk::PipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    colorBlendAttachment.blendEnable = VK_FALSE;

    vk::PipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = vk::LogicOp::eCopy;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::vector<vk::DynamicState> dynamicStates = {vk::DynamicState::eViewport,
                                                   vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    vk::PushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags =
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(BindlessPushConstants);

    std::vector<vk::DescriptorSetLayout> setLayouts = {
        sceneManager->descriptorSetLayout()};
    std::vector<vk::PushConstantRange> pushConstants = {pushConstantRange};
    pipelineLayout =
        pipelineManager->createPipelineLayout(setLayouts, pushConstants);

    vk::GraphicsPipelineCreateInfo pipelineInfo{};
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
    pipelineInfo.renderPass = renderPass.get();
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = vk::Pipeline{};

    graphicsPipeline = pipelineManager->createGraphicsPipeline(
        pipelineInfo, "base_bindless_pipeline");

    deviceWrapper->device().destroyShaderModule(fragShaderModule);
    deviceWrapper->device().destroyShaderModule(vertShaderModule);
  }

  void createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = deviceWrapper->queueFamilyIndices();

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    vk::Device device{deviceWrapper->device()};
    commandPool = device.createCommandPoolUnique(poolInfo);
  }

  void createSceneBuffers() {
    cameraBuffer = allocationManager->createBuffer(
        sizeof(CameraData), vk::BufferUsageFlagBits::eUniformBuffer,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    objectBuffer = allocationManager->createBuffer(
        sizeof(ObjectData) * config_.maxSceneObjects,
        vk::BufferUsageFlagBits::eStorageBuffer, VMA_MEMORY_USAGE_AUTO,
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
    if (cameraBuffer.buffer != vk::Buffer{}) {
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
    if (objectBuffer.buffer == vk::Buffer{}) return;
    syncObjectDataFromSceneGraph();
    if (objectData.empty()) return;
    writeToBuffer(objectBuffer, objectData.data(),
                  sizeof(ObjectData) * objectData.size());
  }

  bool reloadSceneModel(const std::string& path) {
    if (!sceneManager) return false;
    deviceWrapper->device().waitIdle();
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
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = commandPool.get();
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount =
        static_cast<uint32_t>(swapChainManager->imageCount());

    vk::Device device{deviceWrapper->device()};
    commandBuffers = device.allocateCommandBuffersUnique(allocInfo);
  }

  void recreateCommandBuffers() {
    commandBuffers.clear();
    createCommandBuffers();
  }

  void recordCommandBuffer(vk::CommandBuffer commandBuffer,
                           uint32_t imageIndex) {
    vk::CommandBufferBeginInfo beginInfo{};

    if (commandBuffer.begin(&beginInfo) != vk::Result::eSuccess) {
      throw std::runtime_error("failed to begin recording command buffer!");
    }

    vk::RenderPassBeginInfo renderPassInfo{};
    renderPassInfo.renderPass = renderPass.get();
    renderPassInfo.framebuffer = swapChainManager->framebuffers()[imageIndex];
    renderPassInfo.renderArea.offset = vk::Offset2D{0, 0};
    renderPassInfo.renderArea.extent = swapChainManager->extent();

    vk::ClearValue clearColor =
        vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    commandBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                               graphicsPipeline);
    vk::DescriptorSet descriptorSet = sceneManager->descriptorSet();
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                     pipelineLayout, 0, 1, &descriptorSet, 0,
                                     nullptr);

    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapChainManager->extent().width);
    viewport.height = static_cast<float>(swapChainManager->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    commandBuffer.setViewport(0, 1, &viewport);

    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{0, 0};
    scissor.extent = swapChainManager->extent();
    commandBuffer.setScissor(0, 1, &scissor);

    vk::Buffer vertexBuffers[] = {vertexSlice.buffer};
    vk::DeviceSize offsets[] = {vertexSlice.offset};
    commandBuffer.bindVertexBuffers(0, 1, vertexBuffers, offsets);

    commandBuffer.bindIndexBuffer(indexSlice.buffer, indexSlice.offset,
                                  indexType);

    const uint32_t drawCount = static_cast<uint32_t>(
        std::min<size_t>(objectData.size(), config_.maxSceneObjects));
    const std::vector<uint32_t>& indices = sceneManager->indices();
    for (uint32_t i = 0; i < drawCount; ++i) {
      pushConstants.objectIndex = i;
      commandBuffer.pushConstants(
          pipelineLayout,
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
          0, sizeof(BindlessPushConstants), &pushConstants);
      commandBuffer.drawIndexed(static_cast<uint32_t>(indices.size()), 1, 0, 0,
                                0);
    }

    if (guiManager) {
      guiManager->render(commandBuffer);
    }

    commandBuffer.endRenderPass();

    if (commandBuffer.end() != vk::Result::eSuccess) {
      throw std::runtime_error("failed to record command buffer!");
    }
  }

  void drawFrame() {
    frameSyncManager->waitForFrame(currentFrame);

    uint32_t imageIndex;
    vk::Result result = deviceWrapper->device().acquireNextImageKHR(
        swapChainManager->swapChain(), UINT64_MAX,
        frameSyncManager->imageAvailable(currentFrame), vk::Fence{},
        &imageIndex);

    if (result == vk::Result::eErrorOutOfDateKHR) {
      recreateSwapChain();
      return;
    } else if (result != vk::Result::eSuccess &&
               result != vk::Result::eSuboptimalKHR) {
      throw std::runtime_error("failed to acquire swap chain image!");
    }

    if (imagesInFlight[imageIndex]) {
      vk::Fence inFlightFence = imagesInFlight[imageIndex];
      deviceWrapper->device().waitForFences(1, &inFlightFence, VK_TRUE,
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

    commandBuffers[imageIndex].reset(vk::CommandBufferResetFlags{});
    recordCommandBuffer(commandBuffers[imageIndex].get(), imageIndex);

    vk::SubmitInfo submitInfo{};

    vk::Semaphore waitSemaphores[] = {
        frameSyncManager->imageAvailable(currentFrame)};
    vk::PipelineStageFlags waitStages[] = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    vk::CommandBuffer commandBufferHandle = commandBuffers[imageIndex].get();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBufferHandle;

    vk::Semaphore signalSemaphores[] = {
        frameSyncManager->renderFinishedForImage(imageIndex)};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (deviceWrapper->graphicsQueue().submit(
            1, &submitInfo, frameSyncManager->fence(currentFrame)) !=
        vk::Result::eSuccess) {
      throw std::runtime_error("failed to submit draw command buffer!");
    }

    result = swapChainManager->present(
        deviceWrapper->presentQueue(), imageIndex,
        frameSyncManager->renderFinishedForImage(imageIndex));

    if (result == vk::Result::eErrorOutOfDateKHR ||
        result == vk::Result::eSuboptimalKHR || framebufferResized) {
      framebufferResized = false;
      recreateSwapChain();
    } else if (result != vk::Result::eSuccess) {
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
