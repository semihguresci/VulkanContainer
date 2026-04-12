# ─────────────────────────────────────────────────────────────────────────────
# Phase 2: Decomposed utility sub-libraries
#
# The former monolithic VulkanContainer_utility (16 files) is now split into
# six focused libraries.  Header files stay in include/Container/utility/ to
# avoid mass-renaming #include paths across the project.
# ─────────────────────────────────────────────────────────────────────────────

set(_UTILITY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include/")

# ── 1. Logging ───────────────────────────────────────────────────────────────
add_library(VulkanContainer_log
    Logger.cpp
)
target_compile_features(VulkanContainer_log PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_log PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanContainer_log PUBLIC
    Dep_Logging
    Dep_VulkanCore
)

# ── 2. Vulkan device lifecycle ───────────────────────────────────────────────
add_library(VulkanContainer_vulkan_device
    VulkanDevice.cpp
    VulkanInstance.cpp
    SwapChainManager.cpp
    FrameSyncManager.cpp
)
target_compile_features(VulkanContainer_vulkan_device PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_vulkan_device PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanContainer_vulkan_device PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_Windowing
)

# ── 3. GPU resources (memory, buffers, textures, pipelines) ──────────────────
add_library(VulkanContainer_gpu_resource
    AllocationManager.cpp
    VulkanMemoryManager.cpp
    TextureManager.cpp
    PipelineManager.cpp
)
target_compile_features(VulkanContainer_gpu_resource PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_gpu_resource PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanContainer_gpu_resource PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    VulkanContainer_vulkan_device
)

# ── 4. Window + input ───────────────────────────────────────────────────────
add_library(VulkanContainer_window
    WindowManager.cpp
    InputManager.cpp
)
target_compile_features(VulkanContainer_window PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_window PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanContainer_window PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_Windowing
)

# ── 5. Scene (graph, materials, scene manager) ──────────────────────────────
add_library(VulkanContainer_scene
    SceneManager.cpp
    SceneGraph.cpp
    MaterialManager.cpp
    MaterialXIntegration.cpp
)
target_compile_features(VulkanContainer_scene PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_scene PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanContainer_scene PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    Dep_Material
    VulkanContainer_gpu_resource
    VulkanContainer_geometry
)

# ── 6. UI (ImGui) ───────────────────────────────────────────────────────────
add_library(VulkanContainer_ui
    GuiManager.cpp
)
target_compile_features(VulkanContainer_ui PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_ui PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanContainer_ui PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_Windowing
    Dep_UI
    VulkanContainer_scene
)
