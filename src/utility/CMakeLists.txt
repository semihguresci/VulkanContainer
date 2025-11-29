# Utility component
add_library(VulkanContainer_utility
    Logger.cpp
    VkResultToString.cpp
    MaterialXIntegration.cpp
)


target_compile_features(VulkanContainer_utility PUBLIC cxx_std_23)

target_include_directories(VulkanContainer_utility PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(VulkanContainer_utility PUBLIC 
    VulkanDependencies
    spdlog::spdlog 
)