# ── ECS (EnTT-based entity management) ──────────────────────────────────────
add_library(VulkanSceneRenderer_ecs
    World.cpp
)
target_compile_features(VulkanSceneRenderer_ecs PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_ecs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
)
target_link_libraries(VulkanSceneRenderer_ecs PUBLIC
    Dep_ECS
    Dep_Math
    VulkanSceneRenderer_scene
)
