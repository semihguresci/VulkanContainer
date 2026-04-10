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

if(NOT TARGET glslang::validator)
    add_executable(glslang::validator IMPORTED GLOBAL)
endif()

find_program(GLSLANG_VALIDATOR
    glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    REQUIRED
)
set_property(TARGET glslang::validator PROPERTY IMPORTED_LOCATION "${GLSLANG_VALIDATOR}")

find_program(SLANGC_EXECUTABLE
    slangc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    REQUIRED
)

if(SLANGC_EXECUTABLE)
    set(SLANG_MATRIX_LAYOUT_FLAG "-matrix-layout-column-major")
    set(SLANG_SPIRV_FLAGS
        -target spirv
        -profile spirv_1_4
        -emit-spirv-directly
        -fvk-use-entrypoint-name
        -I "${SHADERS_DIR}"
        ${SLANG_MATRIX_LAYOUT_FLAG}
    )
    file(GLOB SLANG_SOURCES CONFIGURE_DEPENDS
        "${SHADERS_DIR}/*.slang"
    )
    list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/surface_normal_common\\.slang$")

    if(SLANG_SOURCES)
        list(LENGTH SLANG_SOURCES SLANG_COUNT)
        message(STATUS "Found ${SLANG_COUNT} Slang shaders in ${SHADERS_DIR}")

        set(SHADER_BUILD_SCRIPT "${CMAKE_BINARY_DIR}/CompileSlangShaders.cmake")
        set(SHADER_BUILD_SCRIPT_CONTENT "")
        string(APPEND SHADER_BUILD_SCRIPT_CONTENT
            "file(REMOVE_RECURSE [=[${COMPILED_SHADERS_DIR}]=])\n"
            "file(MAKE_DIRECTORY [=[${COMPILED_SHADERS_DIR}]=])\n"
        )

        function(append_slang_compile_step SCRIPT_CONTENT_VAR SLANG_SOURCE ENTRY_POINT OUTPUT_PATH)
            set(_script "${${SCRIPT_CONTENT_VAR}}")
            string(APPEND _script
                "message(STATUS \"Compiling ${SLANG_SOURCE} (${ENTRY_POINT})\")\n"
                "execute_process(\n"
                "    COMMAND [=[${SLANGC_EXECUTABLE}]=] [=[${SLANG_SOURCE}]=]"
            )
            foreach(SLANG_FLAG IN LISTS SLANG_SPIRV_FLAGS)
                string(APPEND _script " [=[${SLANG_FLAG}]=]")
            endforeach()
            string(APPEND _script
                " [=[-entry]=] [=[${ENTRY_POINT}]=] [=[-o]=] [=[${OUTPUT_PATH}]=]\n"
                "    RESULT_VARIABLE slang_compile_result\n"
                ")\n"
                "if(NOT slang_compile_result EQUAL 0)\n"
                "    message(FATAL_ERROR \"Failed to compile ${SLANG_SOURCE} (${ENTRY_POINT})\")\n"
                "endif()\n"
            )
            set(${SCRIPT_CONTENT_VAR} "${_script}" PARENT_SCOPE)
        endfunction()

        foreach(SLANG_SOURCE ${SLANG_SOURCES})
            get_filename_component(SHADER_BASE ${SLANG_SOURCE} NAME_WE)
            set(VERT_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.vert.spv")
            set(FRAG_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.frag.spv")

            append_slang_compile_step(
                SHADER_BUILD_SCRIPT_CONTENT "${SLANG_SOURCE}" "vertMain" "${VERT_OUTPUT}")
            append_slang_compile_step(
                SHADER_BUILD_SCRIPT_CONTENT "${SLANG_SOURCE}" "fragMain" "${FRAG_OUTPUT}")
        endforeach()

        set(SURFACE_NORMALS_SOURCE "${SHADERS_DIR}/surface_normals.slang")
        if(EXISTS "${SURFACE_NORMALS_SOURCE}")
            set(SURFACE_NORMALS_GEOM_OUTPUT "${COMPILED_SHADERS_DIR}/surface_normals.geom.spv")
            append_slang_compile_step(
                SHADER_BUILD_SCRIPT_CONTENT "${SURFACE_NORMALS_SOURCE}" "geomMain" "${SURFACE_NORMALS_GEOM_OUTPUT}")
        endif()

        file(WRITE "${SHADER_BUILD_SCRIPT}" "${SHADER_BUILD_SCRIPT_CONTENT}")

        add_custom_target(shaders_all
            COMMAND ${CMAKE_COMMAND} -P "${SHADER_BUILD_SCRIPT}"
            DEPENDS ${SLANG_SOURCES} "${CMAKE_CURRENT_LIST_FILE}"
            COMMENT "Rebuilding all Slang shaders"
            VERBATIM
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


