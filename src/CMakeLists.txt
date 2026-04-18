add_library(VulkanContainer_Core 
    ContainerCore.cpp
    app/Application.cpp
)

# Renderer subsystem
add_library(VulkanContainer_renderer
    renderer/RenderPassManager.cpp
    renderer/OitManager.cpp
    renderer/DebugOverlayRenderer.cpp
    renderer/FrameResourceManager.cpp
    renderer/SceneController.cpp
    renderer/CameraController.cpp
    renderer/LightingManager.cpp
    renderer/EnvironmentManager.cpp
    renderer/TinyExrImpl.cpp
    renderer/GraphicsPipelineBuilder.cpp
    renderer/FrameRecorder.cpp
    renderer/RenderGraph.cpp
    renderer/VulkanContextInitializer.cpp
    renderer/WindowInputBridge.cpp
    renderer/CommandBufferManager.cpp
    renderer/RendererFrontend.cpp
    renderer/ShadowManager.cpp
    renderer/GpuCullManager.cpp
    renderer/BloomManager.cpp
    renderer/VulkanContext.cpp
)

target_compile_features(VulkanContainer_renderer PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(VulkanContainer_renderer PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    VulkanContainer_log
    VulkanContainer_vulkan_device
    VulkanContainer_gpu_resource
    VulkanContainer_window
    VulkanContainer_scene
    VulkanContainer_ui
    VulkanContainer_ecs
)

# Add subdirectories
add_subdirectory(geometry)
add_subdirectory(utility)
add_subdirectory(ecs)

target_link_libraries(VulkanContainer_Core PUBLIC
    VulkanContainer_renderer
    VulkanContainer_geometry
)

target_compile_features(VulkanContainer_Core PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
