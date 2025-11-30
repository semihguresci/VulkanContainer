# CMakeLists.txt in project-root
cmake_minimum_required(VERSION 3.14)
project(VulkanContainer)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Include third-party dependencies
include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)

find_package(Vulkan REQUIRED)

add_subdirectory(src)

# Add source files and executable
add_executable(${PROJECT_NAME} main.cpp)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    VulkanContainer_Core  
    VulkanDependencies
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${Vulkan_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Include the tests configuration
include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)


