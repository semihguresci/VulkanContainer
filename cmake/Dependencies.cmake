# cmake/Dependencies.cmake

if(NOT DEFINED EXTERNAL_DIR)
    set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")
endif()

include(cmake/DependenciesSettings.cmake)
include(cmake/FindVulkan.cmake)

find_package(Vulkan REQUIRED)
find_package(VulkanHeaders CONFIG)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(VulkanDependencies INTERFACE)

target_link_libraries(VulkanDependencies INTERFACE Vulkan::Vulkan)
target_link_libraries(VulkanDependencies INTERFACE Vulkan::Headers)
target_link_libraries(VulkanDependencies INTERFACE volk::volk volk::volk_headers)
 target_link_libraries(VulkanDependencies INTERFACE GPUOpen::VulkanMemoryAllocator)
target_include_directories(VulkanDependencies INTERFACE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(VulkanDependencies INTERFACE glfw)
target_link_libraries(VulkanDependencies INTERFACE glm::glm)
target_link_libraries(VulkanDependencies INTERFACE imgui::imgui)
target_link_libraries(VulkanDependencies INTERFACE fmt::fmt)

# Find and include Boost
#find_package(Boost REQUIRED)
#if(Boost_FOUND)
#    target_include_directories(VulkanDependencies INTERFACE ${Boost_INCLUDE_DIRS})
#endif()


