# cmake/Dependencies.cmake

if(NOT DEFINED EXTERNAL_DIR)
    set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")
endif()

include(cmake/DependenciesSettings.cmake)

# Use custom FindVulkan
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(cmake/FindVulkan.cmake)

# Prefer system Vulkan SDK
set(Vulkan_INCLUDE_DIR "$ENV{VULKAN_SDK}/Include" CACHE PATH "Vulkan include dir")
set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib" CACHE FILEPATH "Vulkan library")
find_package(Vulkan REQUIRED)

if(Vulkan_FOUND)
    message(STATUS "✅ Vulkan was found: ${Vulkan_LIBRARY}")
else()
    message(FATAL_ERROR "❌ Vulkan was NOT found. Please set VULKAN_SDK or install Vulkan SDK.")
endif()

# Other packages from vcpkg
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(VulkanDependencies INTERFACE)

target_link_libraries(VulkanDependencies INTERFACE Vulkan::Vulkan)
target_link_libraries(VulkanDependencies INTERFACE volk::volk volk::volk_headers)
target_link_libraries(VulkanDependencies INTERFACE GPUOpen::VulkanMemoryAllocator)
target_include_directories(VulkanDependencies INTERFACE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(VulkanDependencies INTERFACE glfw)
target_link_libraries(VulkanDependencies INTERFACE glm::glm)
target_link_libraries(VulkanDependencies INTERFACE imgui::imgui)
target_link_libraries(VulkanDependencies INTERFACE fmt::fmt)

# Validation layers disabled by design — don't try to link unless explicitly handled
if(ENABLE_VULKAN_VALIDATION_LAYERS AND DEFINED VulkanValidationLayers_LIBRARY)
    message(WARNING "You enabled validation layers but VulkanValidationLayers_LIBRARY is not defined.")
endif()
