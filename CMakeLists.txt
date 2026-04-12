# CMakeLists.txt in project-root
cmake_minimum_required(VERSION 3.14)

# Ensure MSVC targets link the correct C runtime (DLL variant).
# Required for VS 2026+ / MSVC 19.50+ where auto-linking may not
# inject the CRT libs unless the policy is explicitly set.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(VulkanContainer)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Include third-party dependencies
include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(src)

# Shader compilation and asset generation (modular includes)
include(cmake/Shaders.cmake)
include(cmake/Assets.cmake)

# Main executable
add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
    VulkanContainer_Core
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Wire asset dependencies to the executable
if(TARGET shaders)
    add_dependencies(${PROJECT_NAME} shaders)
endif()
add_dependencies(${PROJECT_NAME} copy_materials)
add_dependencies(${PROJECT_NAME} generate_models)

# Include the tests configuration
if(ENABLE_TESTS)
    include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)
endif()




