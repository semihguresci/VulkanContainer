add_library(VulkanSceneRenderer_Core
    ContainerCore.cpp
    app/Application.cpp
)

# Renderer subsystem
add_library(VulkanSceneRenderer_renderer
    renderer/BimManager.cpp
    renderer/RenderPassManager.cpp
    renderer/OitManager.cpp
    renderer/DebugOverlayRenderer.cpp
    renderer/DebugUiPresenter.cpp
    renderer/FrameResourceManager.cpp
    renderer/SceneController.cpp
    renderer/CameraController.cpp
    renderer/LightingManager.cpp
    renderer/EnvironmentManager.cpp
    renderer/TinyExrImpl.cpp
    renderer/GraphicsPipelineBuilder.cpp
    renderer/FrameRecorder.cpp
    renderer/RenderGraph.cpp
    renderer/RenderPassGpuProfiler.cpp
    renderer/RendererTelemetry.cpp
    renderer/VulkanContextInitializer.cpp
    renderer/WindowInputBridge.cpp
    renderer/CommandBufferManager.cpp
    renderer/RendererFrontend.cpp
    renderer/ShadowManager.cpp
    renderer/ShadowCullManager.cpp
    renderer/GpuCullManager.cpp
    renderer/BloomManager.cpp
    renderer/ExposureManager.cpp
    renderer/VulkanContext.cpp
)

target_compile_features(VulkanSceneRenderer_renderer PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(VulkanSceneRenderer_renderer PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    VulkanSceneRenderer_log
    VulkanSceneRenderer_vulkan_device
    VulkanSceneRenderer_gpu_resource
    VulkanSceneRenderer_window
    VulkanSceneRenderer_scene
    VulkanSceneRenderer_ui
    VulkanSceneRenderer_ecs
)

# Add subdirectories
add_subdirectory(geometry)
add_subdirectory(utility)
add_subdirectory(ecs)

target_link_libraries(VulkanSceneRenderer_Core PUBLIC
    VulkanSceneRenderer_renderer
    VulkanSceneRenderer_geometry
)

target_compile_features(VulkanSceneRenderer_Core PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
