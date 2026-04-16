# cmake/Shaders.cmake
# Slang → SPIR-V shader compilation.

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

if(NOT SLANGC_EXECUTABLE)
    message(WARNING "slangc not found — shader compilation disabled")
    return()
endif()

set(SLANG_MATRIX_LAYOUT_FLAG "-matrix-layout-column-major")
set(SLANG_SPIRV_FLAGS
    -target spirv
    -profile spirv_1_4
    -emit-spirv-directly
    -fvk-use-entrypoint-name
    -I "${SHADERS_DIR}"
    ${SLANG_MATRIX_LAYOUT_FLAG}
)

file(GLOB SLANG_SOURCES CONFIGURE_DEPENDS "${SHADERS_DIR}/*.slang")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/surface_normal_common\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/brdf_common\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/lighting_structs\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/shadow_common\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/tile_light_cull\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/brdf_lut\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/equirect_to_cubemap\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/irradiance_convolution\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/prefilter_specular\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/gtao\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/gtao_blur\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/frustum_cull\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/hiz_generate\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/occlusion_cull\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/bloom_downsample\\.slang$")
list(FILTER SLANG_SOURCES EXCLUDE REGEX ".*/bloom_upsample\\.slang$")

if(NOT SLANG_SOURCES)
    message(WARNING "No Slang shaders found in ${SHADERS_DIR}")
    return()
endif()

list(LENGTH SLANG_SOURCES SLANG_COUNT)
message(STATUS "Found ${SLANG_COUNT} Slang shaders in ${SHADERS_DIR}")

# Build a CMake script that compiles every shader in one pass.
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

# Geometry shaders for specific files.
set(SURFACE_NORMALS_SOURCE "${SHADERS_DIR}/surface_normals.slang")
if(EXISTS "${SURFACE_NORMALS_SOURCE}")
    set(SURFACE_NORMALS_GEOM_OUTPUT "${COMPILED_SHADERS_DIR}/surface_normals.geom.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${SURFACE_NORMALS_SOURCE}" "geomMain" "${SURFACE_NORMALS_GEOM_OUTPUT}")
endif()

set(WIREFRAME_FALLBACK_SOURCE "${SHADERS_DIR}/wireframe_fallback.slang")
if(EXISTS "${WIREFRAME_FALLBACK_SOURCE}")
    set(WIREFRAME_FALLBACK_GEOM_OUTPUT "${COMPILED_SHADERS_DIR}/wireframe_fallback.geom.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${WIREFRAME_FALLBACK_SOURCE}" "geomMain" "${WIREFRAME_FALLBACK_GEOM_OUTPUT}")
endif()

set(NORMAL_VALIDATION_SOURCE "${SHADERS_DIR}/normal_validation.slang")
if(EXISTS "${NORMAL_VALIDATION_SOURCE}")
    set(NORMAL_VALIDATION_GEOM_OUTPUT "${COMPILED_SHADERS_DIR}/normal_validation.geom.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${NORMAL_VALIDATION_SOURCE}" "geomMain" "${NORMAL_VALIDATION_GEOM_OUTPUT}")
endif()

# Compute shaders.
set(TILE_LIGHT_CULL_SOURCE "${SHADERS_DIR}/tile_light_cull.slang")
if(EXISTS "${TILE_LIGHT_CULL_SOURCE}")
    set(TILE_LIGHT_CULL_OUTPUT "${COMPILED_SHADERS_DIR}/tile_light_cull.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${TILE_LIGHT_CULL_SOURCE}" "computeMain" "${TILE_LIGHT_CULL_OUTPUT}")
endif()

set(BRDF_LUT_SOURCE "${SHADERS_DIR}/brdf_lut.slang")
if(EXISTS "${BRDF_LUT_SOURCE}")
    set(BRDF_LUT_OUTPUT "${COMPILED_SHADERS_DIR}/brdf_lut.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${BRDF_LUT_SOURCE}" "computeMain" "${BRDF_LUT_OUTPUT}")
endif()

set(EQUIRECT_TO_CUBEMAP_SOURCE "${SHADERS_DIR}/equirect_to_cubemap.slang")
if(EXISTS "${EQUIRECT_TO_CUBEMAP_SOURCE}")
    set(EQUIRECT_TO_CUBEMAP_OUTPUT "${COMPILED_SHADERS_DIR}/equirect_to_cubemap.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${EQUIRECT_TO_CUBEMAP_SOURCE}" "computeMain" "${EQUIRECT_TO_CUBEMAP_OUTPUT}")
endif()

set(IRRADIANCE_CONV_SOURCE "${SHADERS_DIR}/irradiance_convolution.slang")
if(EXISTS "${IRRADIANCE_CONV_SOURCE}")
    set(IRRADIANCE_CONV_OUTPUT "${COMPILED_SHADERS_DIR}/irradiance_convolution.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${IRRADIANCE_CONV_SOURCE}" "computeMain" "${IRRADIANCE_CONV_OUTPUT}")
endif()

set(PREFILTER_SPEC_SOURCE "${SHADERS_DIR}/prefilter_specular.slang")
if(EXISTS "${PREFILTER_SPEC_SOURCE}")
    set(PREFILTER_SPEC_OUTPUT "${COMPILED_SHADERS_DIR}/prefilter_specular.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${PREFILTER_SPEC_SOURCE}" "computeMain" "${PREFILTER_SPEC_OUTPUT}")
endif()

set(GTAO_SOURCE "${SHADERS_DIR}/gtao.slang")
if(EXISTS "${GTAO_SOURCE}")
    set(GTAO_OUTPUT "${COMPILED_SHADERS_DIR}/gtao.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${GTAO_SOURCE}" "computeMain" "${GTAO_OUTPUT}")
endif()

set(GTAO_BLUR_SOURCE "${SHADERS_DIR}/gtao_blur.slang")
if(EXISTS "${GTAO_BLUR_SOURCE}")
    set(GTAO_BLUR_OUTPUT "${COMPILED_SHADERS_DIR}/gtao_blur.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${GTAO_BLUR_SOURCE}" "computeMain" "${GTAO_BLUR_OUTPUT}")
endif()

set(FRUSTUM_CULL_SOURCE "${SHADERS_DIR}/frustum_cull.slang")
if(EXISTS "${FRUSTUM_CULL_SOURCE}")
    set(FRUSTUM_CULL_OUTPUT "${COMPILED_SHADERS_DIR}/frustum_cull.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${FRUSTUM_CULL_SOURCE}" "computeMain" "${FRUSTUM_CULL_OUTPUT}")
endif()

set(HIZ_GENERATE_SOURCE "${SHADERS_DIR}/hiz_generate.slang")
if(EXISTS "${HIZ_GENERATE_SOURCE}")
    set(HIZ_GENERATE_OUTPUT "${COMPILED_SHADERS_DIR}/hiz_generate.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${HIZ_GENERATE_SOURCE}" "computeMain" "${HIZ_GENERATE_OUTPUT}")
endif()

set(OCCLUSION_CULL_SOURCE "${SHADERS_DIR}/occlusion_cull.slang")
if(EXISTS "${OCCLUSION_CULL_SOURCE}")
    set(OCCLUSION_CULL_OUTPUT "${COMPILED_SHADERS_DIR}/occlusion_cull.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${OCCLUSION_CULL_SOURCE}" "computeMain" "${OCCLUSION_CULL_OUTPUT}")
endif()

set(BLOOM_DOWNSAMPLE_SOURCE "${SHADERS_DIR}/bloom_downsample.slang")
if(EXISTS "${BLOOM_DOWNSAMPLE_SOURCE}")
    set(BLOOM_DOWNSAMPLE_OUTPUT "${COMPILED_SHADERS_DIR}/bloom_downsample.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${BLOOM_DOWNSAMPLE_SOURCE}" "computeMain" "${BLOOM_DOWNSAMPLE_OUTPUT}")
endif()

set(BLOOM_UPSAMPLE_SOURCE "${SHADERS_DIR}/bloom_upsample.slang")
if(EXISTS "${BLOOM_UPSAMPLE_SOURCE}")
    set(BLOOM_UPSAMPLE_OUTPUT "${COMPILED_SHADERS_DIR}/bloom_upsample.comp.spv")
    append_slang_compile_step(
        SHADER_BUILD_SCRIPT_CONTENT "${BLOOM_UPSAMPLE_SOURCE}" "computeMain" "${BLOOM_UPSAMPLE_OUTPUT}")
endif()

file(WRITE "${SHADER_BUILD_SCRIPT}" "${SHADER_BUILD_SCRIPT_CONTENT}")

add_custom_target(shaders_all
    COMMAND ${CMAKE_COMMAND} -P "${SHADER_BUILD_SCRIPT}"
    DEPENDS ${SLANG_SOURCES} "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Rebuilding all Slang shaders"
    VERBATIM
)

add_custom_target(shaders DEPENDS shaders_all)
