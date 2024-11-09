# cmake/Dependencies.cmake

include(cmake/FetchVulkanHeaders.cmake)
include(cmake/FetchGLFW.cmake)
include(cmake/FetchGLM.cmake)
include(cmake/FetchVolk.cmake)
find_package(Vulkan REQUIRED)


add_library(VulkanDependencies INTERFACE)

target_link_libraries(VulkanDependencies INTERFACE Vulkan::Vulkan volk::volk)
target_include_directories(VulkanDependencies INTERFACE ${Vulkan_INCLUDE_DIRS})

target_link_libraries(VulkanDependencies INTERFACE glfw)
target_link_libraries(VulkanDependencies INTERFACE glm::glm)

