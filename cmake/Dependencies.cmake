# cmake/Dependencies.cmake

if(NOT DEFINED EXTERNAL_DIR)
    set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")
endif()

list(APPEND CMAKE_PREFIX_PATH "${EXTERNAL_DIR}")
list(APPEND CMAKE_MODULE_PATH "${EXTERNAL_DIR}")

include(cmake/FetchVulkanHeaders.cmake)
include(cmake/FetchGLFW.cmake)
include(cmake/FetchGLM.cmake)
include(cmake/FetchVolk.cmake)
include(cmake/FetchBoost.cmake)

find_package(Vulkan REQUIRED)


add_library(VulkanDependencies INTERFACE)

target_link_libraries(VulkanDependencies INTERFACE Vulkan::Vulkan volk::volk)
target_include_directories(VulkanDependencies INTERFACE ${Vulkan_INCLUDE_DIRS})

target_link_libraries(VulkanDependencies INTERFACE glfw)
target_link_libraries(VulkanDependencies INTERFACE glm::glm)

# Find and include Boost
find_package(Boost REQUIRED)
if(Boost_FOUND)
    target_include_directories(VulkanDependencies INTERFACE ${Boost_INCLUDE_DIRS})
endif()


