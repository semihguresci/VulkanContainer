# ── ECS (EnTT-based entity management) ──────────────────────────────────────
add_library(VulkanContainer_ecs
    World.cpp
)
target_compile_features(VulkanContainer_ecs PUBLIC cxx_std_23)
target_include_directories(VulkanContainer_ecs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
)
target_link_libraries(VulkanContainer_ecs PUBLIC
    Dep_ECS
    Dep_Math
    VulkanContainer_scene
)
