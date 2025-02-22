# CMakeLists.txt in project-root
cmake_minimum_required(VERSION 3.14)
project(VulkanProject)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include third-party dependencies
include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)

# Find Vulkan SDK
find_package(Vulkan REQUIRED)

# Add source files and executable
add_executable(${PROJECT_NAME} src/main.cpp)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE VulkanDependencies)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${Vulkan_INCLUDE_DIRS})

# Include the shader compilation target
include(cmake/CompileShaders.cmake)

# Include the tests configuration
include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)


