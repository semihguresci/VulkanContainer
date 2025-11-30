#define GLFW_INCLUDE_VULKAN
#include <Container/utility/FrameSyncManager.h>
#include <Container/utility/Logger.h>
#include <Container/utility/MaterialXIntegration.h>
#include <Container/utility/MaterialManager.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneGraph.h>
#include <Container/utility/SwapChainManager.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanInstance.h>
#include <Container/utility/VulkanMemoryManager.h>
#include <Container/utility/WindowManager.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <boost/core/span.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <vk_mem_alloc.h>

const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;

const int MAX_FRAMES_IN_FLIGHT = 2;
constexpr uint32_t MAX_SCENE_OBJECTS = 16;

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"};

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};

const bool enableValidationLayers = true;

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

struct Vertex {
  glm::vec3 pos;
  glm::vec3 color;

  static VkVertexInputBindingDescription getBindingDescription() {
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    return bindingDescription;
  }

  static std::array<VkVertexInputAttributeDescription, 2>
  getAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};

    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, pos);

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, color);

    return attributeDescriptions;
  }
};

const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, {1.0f, 0.5f, 0.2f}},
    {{-0.5f, 0.5f, 0.5f}, {0.2f, 0.8f, 0.5f}}};

const std::vector<uint16_t> indices = {
    // front (+Z)
    4, 5, 6, 6, 7, 4,
    // back (-Z)
    0, 3, 2, 2, 1, 0,
    // left (-X)
    0, 4, 7, 7, 3, 0,
    // right (+X)
    5, 1, 2, 2, 6, 5,
    // top (+Y)
    3, 7, 6, 6, 2, 3,
    // bottom (-Y)
    0, 1, 5, 5, 4, 0};

constexpr VkDeviceSize MAX_VERTEX_ARENA_SIZE = 4 * 1024 * 1024;  // 4 MB
constexpr VkDeviceSize MAX_INDEX_ARENA_SIZE = 2 * 1024 * 1024;   // 2 MB

struct CameraData {
  alignas(16) glm::mat4 viewProj{1.0f};
};

struct ObjectData {
  alignas(16) glm::mat4 model{1.0f};
  alignas(16) glm::vec4 color{1.0f};
};

struct BindlessPushConstants {
  uint32_t objectIndex{0};
};

class HelloTriangleApplication {
 public:
  void run() {
    initWindow();
    initVulkan();
    mainLoop();
    cleanup();
  }

 private:
  std::unique_ptr<window::WindowManager> windowManager;
  std::unique_ptr<window::Window> window;

  std::shared_ptr<utility::vulkan::VulkanInstance> instanceWrapper;
  std::shared_ptr<utility::vulkan::VulkanDevice> deviceWrapper;
  std::unique_ptr<utility::pipeline::PipelineManager> pipelineManager;

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;

  std::unique_ptr<SwapChainManager> swapChainManager;

  VkRenderPass renderPass;
  VkPipelineLayout pipelineLayout;
  VkPipeline graphicsPipeline;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

  utility::materialx::SlangMaterialXBridge materialXBridge;
  utility::material::TextureManager textureManager{};
  utility::material::MaterialManager materialManager{};
  utility::scene::SceneGraph sceneGraph{};
  std::vector<uint32_t> renderableNodes{};
  uint32_t rootNode = utility::scene::SceneGraph::kInvalidNode;
  uint32_t cubeNode = utility::scene::SceneGraph::kInvalidNode;
  glm::vec4 materialBaseColor{1.0f};
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();

  VkCommandPool commandPool;

  std::unique_ptr<utility::memory::VulkanMemoryManager> memoryManager;
  std::unique_ptr<utility::memory::BufferArena> vertexArena;
  std::unique_ptr<utility::memory::BufferArena> indexArena;
  utility::memory::AllocatedBuffer cameraBuffer{};
  utility::memory::AllocatedBuffer objectBuffer{};

  CameraData cameraData{};
  std::vector<ObjectData> objectData;
  BindlessPushConstants pushConstants{};

  utility::memory::BufferSlice vertexSlice{};
  utility::memory::BufferSlice indexSlice{};

  std::vector<VkCommandBuffer> commandBuffers;

  std::unique_ptr<FrameSyncManager> frameSyncManager;
  std::vector<VkFence> imagesInFlight;
  uint32_t currentFrame = 0;

  bool framebufferResized = false;

  void initWindow() {
    windowManager = std::make_unique<window::WindowManager>();
    window = windowManager->createWindow(WIDTH, HEIGHT, "Vulkan");
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
    loadMaterialXMaterial();
    buildSceneGraph();
    createGraphicsPipeline();
    swapChainManager->createFramebuffers(renderPass);
    createCommandPool();
    createMemoryManager();
    createVertexBuffer();
    createIndexBuffer();
    createSceneBuffers();
    createDescriptorPool();
    allocateAndWriteDescriptorSet();
    createCommandBuffers();
    frameSyncManager = std::make_unique<FrameSyncManager>(
        deviceWrapper->device(), MAX_FRAMES_IN_FLIGHT);
    frameSyncManager->initialize(swapChainManager->imageCount());
    imagesInFlight.assign(swapChainManager->imageCount(), VK_NULL_HANDLE);
    utility::logger::ContainerLogger::instance().renderer()->info(
        "Initializing Vulkan renderer");
    utility::logger::ContainerLogger::instance().vulkan()->debug(
        "Debugging Vulkan initialization");
  }

  void mainLoop() {
    while (!window->shouldClose()) {
      window->pollEvents();
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

    if (renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(deviceWrapper->device(), renderPass, nullptr);
      renderPass = VK_NULL_HANDLE;
    }

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

    vkDestroyCommandPool(deviceWrapper->device(), commandPool, nullptr);

    pipelineManager.reset();
    deviceWrapper.reset();

    if (enableValidationLayers) {
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
      swapChainManager->recreate(renderPass);
      frameSyncManager->recreateRenderFinishedSemaphores(
          swapChainManager->imageCount());
      imagesInFlight.assign(swapChainManager->imageCount(), VK_NULL_HANDLE);
      updateCameraBuffer();
    }
  }

  void createInstance() {
    utility::vulkan::InstanceCreateInfo createInfo{};
    createInfo.applicationName = "Hello Triangle";
    createInfo.engineName = "No Engine";
    createInfo.apiVersion = VK_API_VERSION_1_3;
    createInfo.enableValidationLayers = enableValidationLayers;
    createInfo.validationLayers = validationLayers;
    createInfo.requiredExtensions = getRequiredExtensions();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (enableValidationLayers) {
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
    if (!enableValidationLayers) return;

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
    createInfo.requiredExtensions = deviceExtensions;
    createInfo.validationLayers = validationLayers;
    createInfo.enableValidationLayers = enableValidationLayers;

    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
    descriptorIndexingFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descriptorIndexingFeatures.shaderUniformBufferArrayNonUniformIndexing =
        VK_TRUE;
    descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount =
        VK_TRUE;
    descriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind =
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

  void createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding cameraBinding{};
    cameraBinding.binding = 0;
    cameraBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cameraBinding.descriptorCount = 1;
    cameraBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding objectBinding{};
    objectBinding.binding = 1;
    objectBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    objectBinding.descriptorCount = MAX_SCENE_OBJECTS;
    objectBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {cameraBinding,
                                                             objectBinding};

    std::array<VkDescriptorBindingFlags, 2> bindingFlags = {
        0u, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};

    descriptorSetLayout = pipelineManager->createDescriptorSetLayout(
        std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                  bindings.end()),
        std::vector<VkDescriptorBindingFlags>(bindingFlags.begin(),
                                              bindingFlags.end()),
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
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

    auto bindingDescription = Vertex::getBindingDescription();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();

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
      throw std::runtime_error("failed to create graphics command pool!");
    }
  }

  void createMemoryManager() {
    memoryManager = std::make_unique<utility::memory::VulkanMemoryManager>(
        instance, deviceWrapper->physicalDevice(), deviceWrapper->device());

    vertexArena = std::make_unique<utility::memory::BufferArena>(
        *memoryManager, MAX_VERTEX_ARENA_SIZE,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

    indexArena = std::make_unique<utility::memory::BufferArena>(
        *memoryManager, MAX_INDEX_ARENA_SIZE,
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
        sizeof(ObjectData) * MAX_SCENE_OBJECTS,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    updateCameraBuffer();
    updateObjectBuffer();
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
    if (!swapChainManager) return;
    const auto extent = swapChainManager->extent();
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(extent.height);

    const glm::mat4 view =
        glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                    glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
    proj[1][1] *= -1.0f;

    cameraData.viewProj = proj * view;
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

  void syncObjectDataFromSceneGraph() {
    sceneGraph.updateWorldTransforms();
    renderableNodes = sceneGraph.renderableNodes();
    const size_t objectCount =
        std::min<size_t>(renderableNodes.size(), MAX_SCENE_OBJECTS);
    objectData.resize(objectCount);

    for (size_t i = 0; i < objectCount; ++i) {
      const uint32_t nodeIndex = renderableNodes[i];
      const auto* node = sceneGraph.getNode(nodeIndex);
      const glm::mat4 model = node ? node->worldTransform : glm::mat4(1.0f);
      const uint32_t materialIndex = node ? node->materialIndex : defaultMaterialIndex;
      objectData[i].model = model;
      objectData[i].color = resolveMaterialColor(materialIndex);
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
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_SCENE_OBJECTS + 1;

    descriptorPool = pipelineManager->createDescriptorPool(
        std::vector<VkDescriptorPoolSize>{poolSize}, 1,
        VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT);
  }

  void allocateAndWriteDescriptorSet() {
    VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{};
    countInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    countInfo.descriptorSetCount = 1;
    uint32_t descriptorCount = MAX_SCENE_OBJECTS;
    countInfo.pDescriptorCounts = &descriptorCount;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout;
    allocInfo.pNext = &countInfo;

    if (vkAllocateDescriptorSets(deviceWrapper->device(), &allocInfo,
                                 &descriptorSet) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate descriptor set!");
    }

    VkDescriptorBufferInfo cameraBufferInfo{};
    cameraBufferInfo.buffer = cameraBuffer.buffer;
    cameraBufferInfo.offset = 0;
    cameraBufferInfo.range = sizeof(CameraData);

    std::vector<VkDescriptorBufferInfo> objectBufferInfos{};
    if (!objectData.empty()) {
      objectBufferInfos.resize(objectData.size());
      for (size_t i = 0; i < objectData.size(); ++i) {
        objectBufferInfos[i].buffer = objectBuffer.buffer;
        objectBufferInfos[i].offset = static_cast<VkDeviceSize>(
            i * sizeof(ObjectData));
        objectBufferInfos[i].range = sizeof(ObjectData);
      }
    } else {
      objectBufferInfos.push_back({objectBuffer.buffer, 0, sizeof(ObjectData)});
    }

    std::array<VkWriteDescriptorSet, 2> descriptorWrites{};

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = descriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &cameraBufferInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = descriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[1].descriptorCount =
        static_cast<uint32_t>(objectBufferInfos.size());
    descriptorWrites[1].pBufferInfo = objectBufferInfos.data();

    vkUpdateDescriptorSets(deviceWrapper->device(),
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(), 0, nullptr);
  }

  void createVertexBuffer() {
    vertexSlice = uploadBufferToArena(boost::span<const Vertex>(vertices),
                                      *vertexArena, alignof(Vertex));
  }

  void createIndexBuffer() {
    constexpr VkDeviceSize indexAlignment =
        std::max<VkDeviceSize>(sizeof(uint16_t), 4U);
    indexSlice = uploadBufferToArena(boost::span<const uint16_t>(indices),
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

  void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size,
                  VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(deviceWrapper->device(), &allocInfo,
                             &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = srcOffset;
    copyRegion.dstOffset = dstOffset;
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(deviceWrapper->graphicsQueue(), 1, &submitInfo,
                  VK_NULL_HANDLE);
    vkQueueWaitIdle(deviceWrapper->graphicsQueue());

    vkFreeCommandBuffers(deviceWrapper->device(), commandPool, 1,
                         &commandBuffer);
  }

  void createCommandBuffers() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();

    if (vkAllocateCommandBuffers(deviceWrapper->device(), &allocInfo,
                                 commandBuffers.data()) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate command buffers!");
    }
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
                         VK_INDEX_TYPE_UINT16);

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

    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
      vkWaitForFences(deviceWrapper->device(), 1, &imagesInFlight[imageIndex],
                     VK_TRUE, UINT64_MAX);
    }

    frameSyncManager->resetFence(currentFrame);
    imagesInFlight[imageIndex] = frameSyncManager->fence(currentFrame);

    vkResetCommandBuffer(commandBuffers[currentFrame],
                         /*VkCommandBufferResetFlagBits*/ 0);
    recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {
        frameSyncManager->imageAvailable(currentFrame)};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

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

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
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

    if (enableValidationLayers) {
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
  debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                VkDebugUtilsMessageTypeFlagsEXT messageType,
                const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                void* pUserData) {
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
  }
};

int main() {
  HelloTriangleApplication app;

  try {
    app.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}