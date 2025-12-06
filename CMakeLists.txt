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

set(SHADERS_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(COMPILED_SHADERS_DIR "${CMAKE_BINARY_DIR}/spv_shaders")

find_program(SLANGC_EXECUTABLE slangc)

if(NOT SLANGC_EXECUTABLE)
    message(WARNING "slangc not found. Precompiled Slang shaders must exist in ${COMPILED_SHADERS_DIR}.")
else()
    file(GLOB SLANG_SOURCES
        "${SHADERS_DIR}/*.slang"
    )

    if(SLANG_SOURCES)
        list(LENGTH SLANG_SOURCES SLANG_COUNT)
        message(STATUS "Found ${SLANG_COUNT} Slang shaders in ${SHADERS_DIR}")

        add_custom_command(
            OUTPUT "${COMPILED_SHADERS_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${COMPILED_SHADERS_DIR}"
            COMMENT "Creating shader output directory"
        )

        set(COMPILED_SHADERS "")

        foreach(SLANG_SOURCE ${SLANG_SOURCES})
            get_filename_component(SHADER_BASE ${SLANG_SOURCE} NAME_WE)
            set(VERT_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.vert.spv")
            set(FRAG_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.frag.spv")

            add_custom_command(
                OUTPUT "${VERT_OUTPUT}"
                COMMAND ${SLANGC_EXECUTABLE} "${SLANG_SOURCE}" -target spirv -entry vertMain -o "${VERT_OUTPUT}"
                DEPENDS "${SLANG_SOURCE}" "${COMPILED_SHADERS_DIR}"
                COMMENT "Compiling ${SLANG_SOURCE} (vertMain) -> ${VERT_OUTPUT}"
                VERBATIM
            )

            add_custom_command(
                OUTPUT "${FRAG_OUTPUT}"
                COMMAND ${SLANGC_EXECUTABLE} "${SLANG_SOURCE}" -target spirv -entry fragMain -o "${FRAG_OUTPUT}"
                DEPENDS "${SLANG_SOURCE}" "${COMPILED_SHADERS_DIR}"
                COMMENT "Compiling ${SLANG_SOURCE} (fragMain) -> ${FRAG_OUTPUT}"
                VERBATIM
            )

            list(APPEND COMPILED_SHADERS "${VERT_OUTPUT}" "${FRAG_OUTPUT}")
        endforeach()

        add_custom_target(shaders_all
            DEPENDS ${COMPILED_SHADERS}
            COMMENT "Building all Slang shaders"
        )

        add_custom_target(shaders DEPENDS shaders_all)
        add_dependencies(${PROJECT_NAME} shaders)
    else()
        message(WARNING "No Slang shaders found in ${SHADERS_DIR}")
    endif()
endif()

add_custom_target(copy_materials ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/materials
            ${CMAKE_BINARY_DIR}/materials
    COMMENT "Copying MaterialX assets to build directory"
)

add_dependencies(${PROJECT_NAME} copy_materials)

set(MODELS_DIR "${CMAKE_SOURCE_DIR}/models")
set(MODELS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/models")

find_package(Git QUIET)

add_custom_command(
    OUTPUT
        "${MODELS_OUTPUT_DIR}/basic_cube.gltf"
        "${MODELS_OUTPUT_DIR}/cube_baseColor.png"
        "${MODELS_OUTPUT_DIR}/cube_emissive.png"
        "${MODELS_OUTPUT_DIR}/cube_metallicRoughness.png"
        "${MODELS_OUTPUT_DIR}/cube_normal.png"
        "${MODELS_OUTPUT_DIR}/cube_occlusion.png"
        "${MODELS_OUTPUT_DIR}/basic_triangle.gltf"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODELS_OUTPUT_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/generate_pbr_cube_assets.py"
            --output-dir "${MODELS_OUTPUT_DIR}"
            --static-model "${MODELS_DIR}/basic_triangle.gltf"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/cmake/generate_pbr_cube_assets.py"
        "${MODELS_DIR}/basic_triangle.gltf"
    COMMENT "Generating glTF assets with Python"
    VERBATIM
)

set(GENERATE_MODEL_OUTPUTS
    "${MODELS_OUTPUT_DIR}/basic_cube.gltf"
    "${MODELS_OUTPUT_DIR}/cube_baseColor.png"
    "${MODELS_OUTPUT_DIR}/cube_emissive.png"
    "${MODELS_OUTPUT_DIR}/cube_metallicRoughness.png"
    "${MODELS_OUTPUT_DIR}/cube_normal.png"
    "${MODELS_OUTPUT_DIR}/cube_occlusion.png"
    "${MODELS_OUTPUT_DIR}/basic_triangle.gltf"
)

set(GLTF_SAMPLE_MODELS_DIR "${MODELS_OUTPUT_DIR}/glTF-Sample-Models")
set(GLTF_SAMPLE_MODELS_STAMP "${GLTF_SAMPLE_MODELS_DIR}/.fetched")

add_custom_command(
    OUTPUT "${GLTF_SAMPLE_MODELS_STAMP}"
    COMMAND ${CMAKE_COMMAND}
            "-DDESTINATION:PATH=${GLTF_SAMPLE_MODELS_DIR}"
            "-DSTAMP_FILE:FILEPATH=${GLTF_SAMPLE_MODELS_STAMP}"
            -P "${CMAKE_SOURCE_DIR}/cmake/fetch_gltf_sample_models.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/cmake/fetch_gltf_sample_models.cmake"
    COMMENT "Downloading glTF Sample Models repository archive"
    VERBATIM
)

list(APPEND GENERATE_MODEL_OUTPUTS "${GLTF_SAMPLE_MODELS_STAMP}")

add_custom_target(generate_models ALL
    DEPENDS
        ${GENERATE_MODEL_OUTPUTS}
)

add_dependencies(${PROJECT_NAME} generate_models)


# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${Vulkan_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Include the tests configuration
include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)


