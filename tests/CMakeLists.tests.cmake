enable_testing()
find_package(GTest CONFIG REQUIRED)

set(TESTS_DIR "${CMAKE_SOURCE_DIR}/tests/")
set(TEST_RESULTS_DIR "${CMAKE_BINARY_DIR}/test_results")
file(MAKE_DIRECTORY ${TEST_RESULTS_DIR})

set(DEFAULT_SHADER_DIR "${CMAKE_SOURCE_DIR}/tests/shaders")

# ── Helper function ──────────────────────────────────────────────────────────
# add_custom_test(TARGET_NAME SOURCE_FILE SHADER_DIR TEST_RESULTS_DIR [deps...])
#
# Creates a GTest executable with consistent properties.  Dependencies are
# passed as additional arguments after TEST_RESULTS_DIR.
# If SHADER_DIR is empty (""), the post-build shader copy step is skipped.
function(add_custom_test TARGET_NAME SOURCE_FILE SHADER_DIR TEST_RESULTS_DIR)
    set(extra_deps ${ARGN})

    add_executable(${TARGET_NAME} ${SOURCE_FILE})

    target_link_libraries(${TARGET_NAME} PRIVATE
        ${extra_deps}
        GTest::gtest
        GTest::gtest_main
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/tests
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_test(
        NAME ${TARGET_NAME}
        COMMAND ${TARGET_NAME} --gtest_output=xml:${TEST_RESULTS_DIR}/${TARGET_NAME}.xml
    )

    # Copy shaders only when a shader directory is provided.
    if(NOT "${SHADER_DIR}" STREQUAL "")
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${SHADER_DIR}
                $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders
            COMMENT "Copying shaders to output directory for ${TARGET_NAME}"
        )
    endif()
endfunction()

# ── CPU-only tests (no shaders needed) ───────────────────────────────────────

add_custom_test(glm_tests
    ${TESTS_DIR}/glm_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)

add_custom_test(ecs_tests
    ${TESTS_DIR}/ecs_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ecs  VulkanSceneRenderer_scene
)

add_custom_test(scene_graph_tests
    ${TESTS_DIR}/scene_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)

add_custom_test(gltf_loader_tests
    ${TESTS_DIR}/gltf_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(sample_model_regression_tests
    ${TESTS_DIR}/sample_model_regression_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
    nlohmann_json::nlohmann_json
)
target_compile_definitions(sample_model_regression_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CONTAINER_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
add_dependencies(sample_model_regression_tests generate_models)

add_custom_test(materialx_integration_tests
    ${TESTS_DIR}/materialx_integration_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)

add_custom_test(rendering_convention_tests
    ${TESTS_DIR}/rendering_convention_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)

add_custom_test(realistic_rendering_validation_tests
    ${TESTS_DIR}/realistic_rendering_validation_tests.cpp  ""  ${TEST_RESULTS_DIR}
    nlohmann_json::nlohmann_json
)

add_custom_test(visual_regression_gpu_tests
    ${TESTS_DIR}/visual_regression_gpu_tests.cpp  ""  ${TEST_RESULTS_DIR}
    nlohmann_json::nlohmann_json
    VulkanSceneRenderer_geometry
)
target_compile_definitions(visual_regression_gpu_tests PRIVATE
    CONTAINER_APP_EXECUTABLE="$<TARGET_FILE:VulkanSceneRenderer>"
    CONTAINER_TEST_RESULTS_DIR="${TEST_RESULTS_DIR}"
)
add_dependencies(visual_regression_gpu_tests VulkanSceneRenderer)
set_tests_properties(visual_regression_gpu_tests
    PROPERTIES LABELS "requires-vulkan;requires-display;visual-regression")

add_custom_test(render_graph_tests
    ${TESTS_DIR}/render_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

# ── Tests that need Vulkan / windowing runtime ───────────────────────────────

if(ENABLE_WINDOWED_TESTS)
    add_custom_test(triangle_test
        ${TESTS_DIR}/triangle_test.cpp  ${DEFAULT_SHADER_DIR}  ${TEST_RESULTS_DIR}
        Dep_Windowing
    )

    add_custom_test(vulkan_tests
        ${TESTS_DIR}/vulkan_tests.cpp  ${DEFAULT_SHADER_DIR}  ${TEST_RESULTS_DIR}
        Dep_VulkanCore
    )

    add_custom_test(window_creation_test
        ${TESTS_DIR}/window_creation_test.cpp  ${DEFAULT_SHADER_DIR}  ${TEST_RESULTS_DIR}
        Dep_Windowing
    )

    set_tests_properties(triangle_test vulkan_tests window_creation_test
        PROPERTIES LABELS "requires-vulkan;requires-display")
endif()
