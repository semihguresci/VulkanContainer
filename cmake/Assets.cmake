# cmake/Assets.cmake
# Model generation and MaterialX asset copy targets.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# --- MaterialX materials ------------------------------------------------

add_custom_target(copy_materials ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/materials
            ${CMAKE_BINARY_DIR}/materials
    COMMENT "Copying MaterialX assets to build directory"
)

# --- glTF model generation ----------------------------------------------

set(MODELS_DIR "${CMAKE_SOURCE_DIR}/models")
set(MODELS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/models")

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

# --- glTF Sample Models download ----------------------------------------

find_package(Git QUIET)

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
    DEPENDS ${GENERATE_MODEL_OUTPUTS}
)
