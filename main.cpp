#define GLFW_INCLUDE_VULKAN
#include <Container/app/AppConfig.h>
#include <Container/geometry/Model.h>
#include <Container/utility/FrameSyncManager.h>
#include <Container/utility/Camera.h>
#include <Container/utility/InputManager.h>
#include <Container/utility/Logger.h>
#include <Container/utility/MaterialManager.h>
#include <Container/utility/MaterialXIntegration.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneGraph.h>
#include <Container/utility/SwapChainManager.h>
#include <Container/utility/VulkanAlignment.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanHandles.h>
#include <Container/utility/VulkanInstance.h>
#include <Container/utility/VulkanMemoryManager.h>
#include <Container/utility/WindowManager.h>
#include <GLFW/glfw3.h>
#include <tiny_gltf.h>

#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <array>
#include <boost/core/span.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <limits>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>

#include <stb_image.h>

using utility::FrameSyncManager;
using utility::QueueFamilyIndices;
using utility::SwapChainManager;
using utility::SwapChainSupportDetails;
namespace window = utility::window;

VkResult CreateDebugUtilsMessengerEXT(
    VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pDebugMessenger) {
  auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
      instance, "vkCreateDebugUtilsMessengerEXT");
  if (func != nullptr) {
    return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
  } else {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance,
                                   VkDebugUtilsMessengerEXT debugMessenger,
                                   const VkAllocationCallbacks* pAllocator) {
  auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
      instance, "vkDestroyDebugUtilsMessengerEXT");
  if (func != nullptr) {
    func(instance, debugMessenger, pAllocator);
  }
}


struct CameraData {
  alignas(16) glm::mat4 viewProj{1.0f};
};

struct ObjectData {
  alignas(16) glm::mat4 model{1.0f};
  alignas(16) glm::vec4 color{1.0f};
  alignas(16) glm::vec3 emissiveColor{0.0f, 0.0f, 0.0f};
  alignas(4) float emissiveStrength{1.0f};
  alignas(8) glm::vec2 metallicRoughness{1.0f, 1.0f};
  alignas(4) uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t padding{0};
};

struct BindlessPushConstants {
  uint32_t objectIndex{0};
};

class HelloTriangleApplication {
 public:
  explicit HelloTriangleApplication(app::AppConfig config)
      : config_(std::move(config)) {}

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

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;

  std::unique_ptr<SwapChainManager> swapChainManager;

  utility::vulkan::UniqueRenderPass renderPass;
  VkPipelineLayout pipelineLayout;
  VkPipeline graphicsPipeline;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkSampler baseColorSampler = VK_NULL_HANDLE;

  utility::materialx::SlangMaterialXBridge materialXBridge;
  utility::material::TextureManager textureManager{};
  utility::material::MaterialManager materialManager{};
  std::vector<VmaAllocation> textureAllocations{};
  std::vector<VkImage> textureImages{};
  utility::scene::SceneGraph sceneGraph{};
  std::vector<uint32_t> renderableNodes{};
  uint32_t rootNode = utility::scene::SceneGraph::kInvalidNode;
  uint32_t cubeNode = utility::scene::SceneGraph::kInvalidNode;
  glm::vec4 materialBaseColor{1.0f};
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  geometry::Model model{};
  tinygltf::Model gltfModel{};
  std::vector<geometry::Vertex> vertices{};
  std::vector<uint32_t> indices{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};

  utility::vulkan::UniqueCommandPool commandPool;
  std::vector<vk::UniqueCommandBuffer> commandBuffers;

  std::unique_ptr<utility::memory::VulkanMemoryManager> memoryManager;
  std::unique_ptr<utility::memory::BufferArena> vertexArena;
  std::unique_ptr<utility::memory::BufferArena> indexArena;
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
    createDescriptorSetLayout();
    createSampler();
    loadMaterialXMaterial();
    createGraphicsPipeline();
    swapChainManager->createFramebuffers(renderPass.get());
    createCommandPool();
    createMemoryManager();
    loadGltfAssets();
    createVertexBuffer();
    createIndexBuffer();
    buildSceneGraph();
    createCamera();
    createSceneBuffers();
    createDescriptorPool();
    allocateAndWriteDescriptorSet();
    createCommandBuffers();
    frameSyncManager = std::make_unique<FrameSyncManager>(
        vk::Device{deviceWrapper->device()}, config_.maxFramesInFlight);
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

    vkDeviceWaitIdle(deviceWrapper->device());
  }

  void cleanup() {
    if (swapChainManager) {
      swapChainManager->cleanup();
    }

    if (pipelineManager) {
      pipelineManager->destroyManagedResources();
      graphicsPipeline = VK_NULL_HANDLE;
      pipelineLayout = VK_NULL_HANDLE;
      descriptorPool = VK_NULL_HANDLE;
      descriptorSetLayout = VK_NULL_HANDLE;
    }

    if (baseColorSampler != VK_NULL_HANDLE) {
      vkDestroySampler(deviceWrapper->device(), baseColorSampler, nullptr);
      baseColorSampler = VK_NULL_HANDLE;
    }

    for (size_t i = 0; i < textureImages.size(); ++i) {
      if (textureManager.getTexture(static_cast<uint32_t>(i)) != nullptr) {
        vkDestroyImageView(deviceWrapper->device(),
                          textureManager.getTexture(static_cast<uint32_t>(i))->imageView,
                          nullptr);
      }
    }
    for (auto image : textureImages) {
      if (image != VK_NULL_HANDLE) {
        vkDestroyImage(deviceWrapper->device(), image, nullptr);
      }
    }
    for (auto allocation : textureAllocations) {
      if (allocation != VK_NULL_HANDLE && memoryManager) {
        vmaFreeMemory(memoryManager->allocator(), allocation);
      }
    }

    renderPass.reset();

    if (memoryManager) {
      if (cameraBuffer.buffer != VK_NULL_HANDLE) {
        memoryManager->destroyBuffer(cameraBuffer);
      }
      if (objectBuffer.buffer != VK_NULL_HANDLE) {
        memoryManager->destroyBuffer(objectBuffer);
      }
      indexArena.reset();
      vertexArena.reset();
      memoryManager.reset();
    }

    frameSyncManager.reset();

    commandPool.reset();

    pipelineManager.reset();
    deviceWrapper.reset();

    if (config_.enableValidationLayers) {
      DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    }

    vkDestroySurfaceKHR(instance, surface, nullptr);
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
      frameSyncManager->recreateRenderFinishedSemaphores(
          swapChainManager->imageCount());
      imagesInFlight.assign(swapChainManager->imageCount(), vk::Fence{});
      updateCameraBuffer();
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

  void createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding cameraBinding{};
    cameraBinding.binding = 0;
    cameraBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cameraBinding.descriptorCount = 1;
    cameraBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding objectBinding{};
    objectBinding.binding = 1;
    objectBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    objectBinding.descriptorCount = 1;
    objectBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 2;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 3;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    textureBinding.descriptorCount = config_.maxSceneObjects;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
        cameraBinding, objectBinding, samplerBinding, textureBinding};

    std::array<VkDescriptorBindingFlags, 4> bindingFlags = {
        0u,
        0u,
        0u,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};

    descriptorSetLayout = pipelineManager->createDescriptorSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                  bindings.end()),
        std::vector<VkDescriptorBindingFlags>(bindingFlags.begin(),
                                              bindingFlags.end()),
        0);
  }

  void createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.mipLodBias = 0.0f;

    if (vkCreateSampler(deviceWrapper->device(), &samplerInfo, nullptr,
                        &baseColorSampler) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture sampler!");
    }
  }

  void loadMaterialXMaterial() {
    utility::material::Material material{};
    try {
      auto document = materialXBridge.loadDocument("materials/base.mtlx");
      material.baseColor = materialXBridge.extractBaseColor(document);
    } catch (const std::exception& exc) {
      std::cerr << "MaterialX load failed: " << exc.what() << std::endl;
      material.baseColor = glm::vec4(1.0f);
    }

    material.emissiveColor = glm::vec3(0.0f);
    material.metallicFactor = 1.0f;
    material.roughnessFactor = 1.0f;

    materialBaseColor = material.baseColor;
    if (defaultMaterialIndex == std::numeric_limits<uint32_t>::max()) {
      defaultMaterialIndex = materialManager.createMaterial(material);
    } else {
      materialManager.updateMaterial(defaultMaterialIndex, material);
    }
  }

  void buildSceneGraph() {
    rootNode = sceneGraph.createNode(glm::mat4(1.0f), defaultMaterialIndex, false);
    cubeNode = sceneGraph.createNode(glm::mat4(1.0f), defaultMaterialIndex, true);
    sceneGraph.setParent(cubeNode, rootNode);
    sceneGraph.updateWorldTransforms();
    renderableNodes = sceneGraph.renderableNodes();
  }

  void createGraphicsPipeline() {
    auto vertShaderCode = readFile("spv_shaders/base.vert.spv");
    auto fragShaderCode = readFile("spv_shaders/base.frag.spv");

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

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

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo,
                                                      fragShaderStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    auto bindingDescription = geometry::Vertex::bindingDescription();
    auto attributeDescriptions = geometry::Vertex::attributeDescriptions();

    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
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
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(BindlessPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = {descriptorSetLayout};
    std::vector<VkPushConstantRange> pushConstants = {pushConstantRange};
    pipelineLayout = pipelineManager->createPipelineLayout(setLayouts,
                                                           pushConstants);

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
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
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    graphicsPipeline = pipelineManager->createGraphicsPipeline(
        pipelineInfo, "base_bindless_pipeline");

    vkDestroyShaderModule(deviceWrapper->device(), fragShaderModule, nullptr);
    vkDestroyShaderModule(deviceWrapper->device(), vertShaderModule, nullptr);
  }

  void createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = deviceWrapper->queueFamilyIndices();

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    vk::Device device{deviceWrapper->device()};
    commandPool = device.createCommandPoolUnique(poolInfo);
  }

  void createMemoryManager() {
    memoryManager = std::make_unique<utility::memory::VulkanMemoryManager>(
        instance, deviceWrapper->physicalDevice(), deviceWrapper->device());

    vertexArena = std::make_unique<utility::memory::BufferArena>(
        *memoryManager, config_.maxVertexArenaSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

    indexArena = std::make_unique<utility::memory::BufferArena>(
        *memoryManager, config_.maxIndexArenaSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
  }

  void createSceneBuffers() {
    cameraBuffer = memoryManager->createBuffer(
        sizeof(CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    objectBuffer = memoryManager->createBuffer(
        sizeof(ObjectData) * config_.maxSceneObjects,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
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

    const bool cameraChanged = inputManager.update(deltaTime);
    if (cameraChanged) {
      updateCameraBuffer();
    }
  }

  void writeToBuffer(const utility::memory::AllocatedBuffer& buffer,
                     const void* data, size_t size) {
    void* mapped = buffer.allocation_info.pMappedData;
    bool mappedHere = false;
    if (mapped == nullptr) {
      if (vmaMapMemory(memoryManager->allocator(), buffer.allocation, &mapped) !=
          VK_SUCCESS) {
        throw std::runtime_error("failed to map buffer for writing");
      }
      mappedHere = true;
    }

    std::memcpy(mapped, data, size);

    if (mappedHere) {
      vmaUnmapMemory(memoryManager->allocator(), buffer.allocation);
    }
  }

  void updateCameraBuffer() {
    if (!swapChainManager || !camera) return;
    const auto extent = swapChainManager->extent();
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(extent.height);

    cameraData.viewProj = camera->viewProjection(aspect);
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      writeToBuffer(cameraBuffer, &cameraData, sizeof(CameraData));
    }
  }

  glm::vec4 resolveMaterialColor(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return material->baseColor;
    }
    return currentMaterialBaseColor();
  }

  uint32_t resolveMaterialTextureIndex(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return material->baseColorTextureIndex;
    }
    return std::numeric_limits<uint32_t>::max();
  }

  uint32_t resolveMaterialNormalTexture(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return material->normalTextureIndex;
    }
    return std::numeric_limits<uint32_t>::max();
  }

  uint32_t resolveMaterialOcclusionTexture(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return material->occlusionTextureIndex;
    }
    return std::numeric_limits<uint32_t>::max();
  }

  uint32_t resolveMaterialEmissiveTexture(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return material->emissiveTextureIndex;
    }
    return std::numeric_limits<uint32_t>::max();
  }

  uint32_t resolveMaterialMetallicRoughnessTexture(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return material->metallicRoughnessTextureIndex;
    }
    return std::numeric_limits<uint32_t>::max();
  }

  glm::vec2 resolveMaterialMetallicRoughnessFactors(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return glm::vec2(material->metallicFactor, material->roughnessFactor);
    }
    return glm::vec2(1.0f, 1.0f);
  }

  glm::vec4 resolveMaterialEmissive(uint32_t materialIndex) const {
    if (const auto* material = materialManager.getMaterial(materialIndex)) {
      return glm::vec4(material->emissiveColor, 1.0f);
    }
    return glm::vec4(0.0f);
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
      const uint32_t materialIndex = node ? node->materialIndex : defaultMaterialIndex;
      objectData[i].model = model;
      objectData[i].color = resolveMaterialColor(materialIndex);
      objectData[i].emissiveColor = resolveMaterialEmissive(materialIndex);
      objectData[i].metallicRoughness = resolveMaterialMetallicRoughnessFactors(materialIndex);
      objectData[i].baseColorTextureIndex = resolveMaterialTextureIndex(materialIndex);
      objectData[i].normalTextureIndex = resolveMaterialNormalTexture(materialIndex);
      objectData[i].occlusionTextureIndex = resolveMaterialOcclusionTexture(materialIndex);
      objectData[i].emissiveTextureIndex = resolveMaterialEmissiveTexture(materialIndex);
      objectData[i].metallicRoughnessTextureIndex =
          resolveMaterialMetallicRoughnessTexture(materialIndex);
    }
  }

  void updateObjectBuffer() {
    if (objectBuffer.buffer == VK_NULL_HANDLE) return;
    syncObjectDataFromSceneGraph();
    if (objectData.empty()) return;
    writeToBuffer(objectBuffer, objectData.data(),
                  sizeof(ObjectData) * objectData.size());
  }

  glm::vec4 currentMaterialBaseColor() const {
    const auto* material = materialManager.getMaterial(defaultMaterialIndex);
    if (material != nullptr) return material->baseColor;
    return materialBaseColor;
  }

  void createDescriptorPool() {
    VkDescriptorPoolSize uniformPool{};
    uniformPool.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformPool.descriptorCount = config_.maxSceneObjects + 1;

    VkDescriptorPoolSize storagePool{};
    storagePool.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storagePool.descriptorCount = 1;

    VkDescriptorPoolSize sampledImagePool{};
    sampledImagePool.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sampledImagePool.descriptorCount =
        std::max<uint32_t>(config_.maxSceneObjects,
                           static_cast<uint32_t>(textureManager.textureCount()));

    VkDescriptorPoolSize samplerPool{};
    samplerPool.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerPool.descriptorCount = 1;

    descriptorPool = pipelineManager->createDescriptorPool(
        std::vector<VkDescriptorPoolSize>{uniformPool, storagePool, sampledImagePool,
                                          samplerPool},
        1, 0);
  }

  void allocateAndWriteDescriptorSet() {
    const uint32_t textureDescriptorCount = std::min<uint32_t>(
        config_.maxSceneObjects,
        std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager.textureCount())));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout;
    uint32_t descriptorCount = textureDescriptorCount;
    VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{};
    countInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    countInfo.descriptorSetCount = 1;
    countInfo.pDescriptorCounts = &descriptorCount;
    allocInfo.pNext = &countInfo;

    if (vkAllocateDescriptorSets(deviceWrapper->device(), &allocInfo,
                                 &descriptorSet) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate descriptor set!");
    }

    VkDescriptorBufferInfo cameraBufferInfo{};
    cameraBufferInfo.buffer = cameraBuffer.buffer;
    cameraBufferInfo.offset = 0;
    cameraBufferInfo.range = sizeof(CameraData);

    VkDescriptorBufferInfo objectBufferInfo{};
    objectBufferInfo.buffer = objectBuffer.buffer;
    objectBufferInfo.offset = 0;
    objectBufferInfo.range = sizeof(ObjectData) * config_.maxSceneObjects;

    std::vector<VkWriteDescriptorSet> descriptorWrites{};

    VkWriteDescriptorSet cameraWrite{};
    cameraWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cameraWrite.dstSet = descriptorSet;
    cameraWrite.dstBinding = 0;
    cameraWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cameraWrite.descriptorCount = 1;
    cameraWrite.pBufferInfo = &cameraBufferInfo;
    descriptorWrites.push_back(cameraWrite);

    VkWriteDescriptorSet objectWrite{};
    objectWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    objectWrite.dstSet = descriptorSet;
    objectWrite.dstBinding = 1;
    objectWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    objectWrite.descriptorCount = 1;
    objectWrite.pBufferInfo = &objectBufferInfo;
    descriptorWrites.push_back(objectWrite);

    std::vector<VkDescriptorImageInfo> textureInfos{};
    const size_t textureCount = textureManager.textureCount();
    if (textureCount > 0) {
      textureInfos.reserve(std::min<size_t>(textureCount, textureDescriptorCount));
      const size_t maxTextures = std::min<size_t>(textureDescriptorCount, textureCount);
      for (size_t i = 0; i < maxTextures; ++i) {
        const auto* tex = textureManager.getTexture(static_cast<uint32_t>(i));
        if (tex == nullptr) continue;
        VkDescriptorImageInfo info{};
        info.sampler = VK_NULL_HANDLE;
        info.imageView = tex->imageView;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        textureInfos.push_back(info);
      }

      if (!textureInfos.empty()) {
        VkWriteDescriptorSet textureWrite{};
        textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        textureWrite.dstSet = descriptorSet;
        textureWrite.dstBinding = 3;
        textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        textureWrite.descriptorCount =
            static_cast<uint32_t>(textureInfos.size());
        textureWrite.pImageInfo = textureInfos.data();
        descriptorWrites.push_back(textureWrite);
      }
    }

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = baseColorSampler;

    VkWriteDescriptorSet samplerWrite{};
    samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    samplerWrite.dstSet = descriptorSet;
    samplerWrite.dstBinding = 2;
    samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerWrite.descriptorCount = 1;
    samplerWrite.pImageInfo = &samplerInfo;
    descriptorWrites.push_back(samplerWrite);

    vkUpdateDescriptorSets(deviceWrapper->device(),
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(), 0, nullptr);
  }

  void loadGltfAssets() {
    model = geometry::Model::MakeCube();
    gltfModel = tinygltf::Model{};

    if (!config_.modelPath.empty()) {
      try {
        auto gltfResult = geometry::gltf::LoadModelWithSource(config_.modelPath);
        gltfModel = std::move(gltfResult.gltfModel);
        model = std::move(gltfResult.model);

        const auto baseDir = std::filesystem::path(config_.modelPath).parent_path();
        auto imageToTexture = materialXBridge.loadTexturesForGltf(
            gltfModel, baseDir, textureManager,
            [this](const std::string& path) { return createTextureFromFile(path); });
        materialXBridge.loadMaterialsForGltf(gltfModel, imageToTexture,
                                             materialManager, defaultMaterialIndex);
      } catch (const std::exception& exc) {
        std::cerr << "glTF load failed: " << exc.what()
                  << "; falling back to cube model." << std::endl;
      }
    }

    if (model.empty()) {
      model = geometry::Model::MakeCube();
    }

    vertices = model.vertices();
    indices = model.indices();
    indexType = VK_INDEX_TYPE_UINT32;
  }

  void createVertexBuffer() {
    vertexSlice = uploadBufferToArena(boost::span<const geometry::Vertex>(vertices),
                                      *vertexArena, alignof(geometry::Vertex));
  }

  void createIndexBuffer() {
    constexpr VkDeviceSize indexAlignment =
        std::max<VkDeviceSize>(sizeof(uint32_t), 4U);
    indexSlice = uploadBufferToArena(boost::span<const uint32_t>(indices),
                                     *indexArena, indexAlignment);
  }

  template <typename T>
  utility::memory::BufferSlice uploadBufferToArena(
      boost::span<const T> source, utility::memory::BufferArena& arena,
      VkDeviceSize alignment) {
    const VkDeviceSize bufferSize = sizeof(T) * source.size();
    utility::memory::StagingBuffer stagingBuffer(*memoryManager, bufferSize);
    stagingBuffer.upload(boost::span<const std::byte>(
        reinterpret_cast<const std::byte*>(source.data()),
        static_cast<std::size_t>(bufferSize)));

    auto slice = arena.allocate(bufferSize, alignment);
    copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
               slice.offset);
    return slice;
  }

  VkCommandBuffer beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool.get();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(deviceWrapper->device(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
  }

  void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(deviceWrapper->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(deviceWrapper->graphicsQueue());

    vkFreeCommandBuffers(deviceWrapper->device(), commandPool.get(), 1,
                         &commandBuffer);
  }

  void transitionImageLayout(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

      sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

      sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
      throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    endSingleTimeCommands(commandBuffer);
  }

  void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                         uint32_t height) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    endSingleTimeCommands(commandBuffer);
  }

  void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size,
                  VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0) {
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandPool = commandPool.get();
    allocInfo.commandBufferCount = 1;

    vk::Device device{deviceWrapper->device()};
    auto commandBuffers = device.allocateCommandBuffersUnique(allocInfo);
    auto& commandBuffer = commandBuffers.front();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer.get(), &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = srcOffset;
    copyRegion.dstOffset = dstOffset;
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer.get(), srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer.get());

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkCommandBuffer commandBufferHandle = commandBuffer.get();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBufferHandle;

    vkQueueSubmit(deviceWrapper->graphicsQueue(), 1, &submitInfo,
                  VK_NULL_HANDLE);
    vkQueueWaitIdle(deviceWrapper->graphicsQueue());
  }

  VkImageView createImageView(VkImage image, VkFormat format) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(deviceWrapper->device(), &viewInfo, nullptr,
                          &imageView) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture image view!");
    }

    return imageView;
  }

  utility::material::TextureResource createTextureFromFile(
      const std::string& texturePath) {
    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(texturePath.c_str(), &texWidth, &texHeight,
                                &texChannels, STBI_rgb_alpha);
    if (!pixels) {
      throw std::runtime_error("failed to load texture image: " + texturePath);
    }

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) *
                             static_cast<VkDeviceSize>(texHeight) * 4;
    utility::memory::StagingBuffer stagingBuffer(*memoryManager, imageSize);
    stagingBuffer.upload(
        {reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(imageSize)});
    stbi_image_free(pixels);

    VkImage textureImage;
    VmaAllocation textureAllocation;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(texWidth);
    imageInfo.extent.height = static_cast<uint32_t>(texHeight);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(memoryManager->allocator(), &imageInfo, &allocInfo,
                       &textureImage, &textureAllocation, nullptr) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to create texture image!");
    }

    transitionImageLayout(textureImage, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer.buffer().buffer, textureImage,
                      static_cast<uint32_t>(texWidth),
                      static_cast<uint32_t>(texHeight));
    transitionImageLayout(textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkImageView imageView = createImageView(textureImage, imageInfo.format);

    textureImages.push_back(textureImage);
    textureAllocations.push_back(textureAllocation);

    utility::material::TextureResource resource{};
    resource.image = textureImage;
    resource.imageView = imageView;
    resource.name = texturePath;
    return resource;
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

  void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      throw std::runtime_error("failed to begin recording command buffer!");
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass.get();
    renderPassInfo.framebuffer = swapChainManager->framebuffers()[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChainManager->extent();

    VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      graphicsPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    pushConstants.objectIndex = 0;
    vkCmdPushConstants(commandBuffer, pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(BindlessPushConstants), &pushConstants);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapChainManager->extent().width;
    viewport.height = (float)swapChainManager->extent().height;
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

    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 1, 0,
                     0, 0);

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
      auto inFlightFence = static_cast<VkFence>(imagesInFlight[imageIndex]);
      vkWaitForFences(deviceWrapper->device(), 1, &inFlightFence, VK_TRUE,
                     UINT64_MAX);
    }

    frameSyncManager->resetFence(currentFrame);
    imagesInFlight[imageIndex] = frameSyncManager->fence(currentFrame);

    vkResetCommandBuffer(commandBuffers[imageIndex].get(),
                         /*VkCommandBufferResetFlagBits*/ 0);
    recordCommandBuffer(commandBuffers[imageIndex].get(), imageIndex);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {
        frameSyncManager->imageAvailable(currentFrame)};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    VkCommandBuffer commandBufferHandle = commandBuffers[imageIndex].get();
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

  VkShaderModule createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(deviceWrapper->device(), &createInfo, nullptr,
                             &shaderModule) != VK_SUCCESS) {
      throw std::runtime_error("failed to create shader module!");
    }

    return shaderModule;
  }

  std::vector<const char*> getRequiredExtensions() {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions,
                                        glfwExtensions + glfwExtensionCount);

    if (config_.enableValidationLayers) {
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
  }

  static std::vector<char> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
      throw std::runtime_error("failed to open file!");
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);

    file.close();

    return buffer;
  }

  static VKAPI_ATTR VkBool32 VKAPI_CALL
  debugCallback([[maybe_unused]] VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT messageType,
                const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                [[maybe_unused]] void* pUserData) {
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
  }
};

int main() {
  HelloTriangleApplication app{app::DefaultAppConfig()};

  try {
    app.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}