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
find_package(Python3 COMPONENTS Interpreter REQUIRED)

find_package(Vulkan REQUIRED)

add_subdirectory(src)

# Add source files and executable
add_executable(${PROJECT_NAME} main.cpp)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    VulkanContainer_Core
    VulkanDependencies
)

# Find slangc executable
find_program(SLANGC_EXECUTABLE slangc HINTS $ENV{VULKAN_SDK}/bin)
if(NOT SLANGC_EXECUTABLE)
    message(WARNING "slangc not found. Please ensure VULKAN_SDK is set correctly.")
    return()
endif()

set(SHADERS_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(COMPILED_SHADERS_DIR "${CMAKE_BINARY_DIR}/spv_shaders")

# Find all .slang files
file(GLOB SLANG_SOURCES
    "${SHADERS_DIR}/*.slang"
)

if(NOT SLANG_SOURCES)
    message(WARNING "No .slang shaders found in ${SHADERS_DIR}")
    return()
endif()

list(LENGTH SLANG_SOURCES SLANG_COUNT)
message(STATUS "Found ${SLANG_COUNT} SLANG shaders in ${SHADERS_DIR}")

# Create output directory
add_custom_command(
    OUTPUT "${COMPILED_SHADERS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${COMPILED_SHADERS_DIR}"
    COMMENT "Creating shader output directory"
)

set(COMPILED_SHADERS "")

foreach(SLANG_SOURCE ${SLANG_SOURCES})
    get_filename_component(SHADER_NAME ${SLANG_SOURCE} NAME_WE)
    set(SLANG_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_NAME}.spv")

    set(ENTRY_POINTS
        -entry vertMain
        -entry fragMain
    )

    add_custom_command(
        OUTPUT "${SLANG_OUTPUT}"
        COMMAND ${SLANGC_EXECUTABLE}
                "${SLANG_SOURCE}"
                -target spirv
                -profile spirv_1_4+spvRayQueryKHR
                -emit-spirv-directly
                -fvk-use-entrypoint-name
                ${ENTRY_POINTS}
                -o "${SLANG_OUTPUT}"
        DEPENDS "${SLANG_SOURCE}" "${COMPILED_SHADERS_DIR}"
        COMMENT "Compiling ${SHADER_NAME}.slang -> ${SHADER_NAME}.spv"
        VERBATIM
    )

    list(APPEND COMPILED_SHADERS "${SLANG_OUTPUT}")
endforeach()

if(COMPILED_SHADERS)
    add_custom_target(shaders_all
        DEPENDS ${COMPILED_SHADERS}
        COMMENT "Building all SLANG shaders"
    )

    add_custom_target(shaders DEPENDS shaders_all)
endif()

add_dependencies(${PROJECT_NAME} shaders)


# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${Vulkan_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Include the tests configuration
include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)


