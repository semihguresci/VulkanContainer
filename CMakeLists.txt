# CMakeLists.txt in project-root
cmake_minimum_required(VERSION 3.14)
project(VulkanProject)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include third-party dependencies
include(cmake/Options.cmake)
include(cmake/DependenciesSettings.cmake)
include(cmake/Dependencies.cmake)

# Find Vulkan SDK
find_package(Vulkan REQUIRED)

# Add source files and executable
add_executable(VulkanProject src/main.cpp)

# Include the shader compilation target
include(cmake/CompileShaders.cmake)
add_dependencies(VulkanProject CompileShaders)

# Link libraries
target_link_libraries(VulkanProject PRIVATE VulkanDependencies)

# Include directories
target_include_directories(VulkanProject PRIVATE ${Vulkan_INCLUDE_DIRS})

# Include the tests configuration
if(ENABLE_TESTS)
    include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)
endif()


