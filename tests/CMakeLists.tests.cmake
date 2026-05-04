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

add_custom_test(input_manager_tests
    ${TESTS_DIR}/input_manager_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_window
)

add_custom_test(gltf_loader_tests
    ${TESTS_DIR}/gltf_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(dotbim_loader_tests
    ${TESTS_DIR}/dotbim_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(ifc_tessellated_loader_tests
    ${TESTS_DIR}/ifc_tessellated_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(ifcx_loader_tests
    ${TESTS_DIR}/ifcx_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
target_compile_definitions(ifcx_loader_tests PRIVATE
    CONTAINER_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
add_dependencies(ifcx_loader_tests generate_models)

add_custom_test(usd_loader_tests
    ${TESTS_DIR}/usd_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
target_compile_definitions(usd_loader_tests PRIVATE
    CONTAINER_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
add_dependencies(usd_loader_tests generate_models)

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

add_custom_test(bcf_viewpoint_tests
    ${TESTS_DIR}/bcf_viewpoint_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ui
)

add_custom_test(rendering_convention_tests
    ${TESTS_DIR}/rendering_convention_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(rendering_convention_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimSectionCapBuilder.cpp
)

add_custom_test(render_technique_registry_tests
    ${TESTS_DIR}/render_technique_registry_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(render_technique_registry_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(deferred_raster_post_process_tests
    ${TESTS_DIR}/deferred_raster_post_process_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_lighting_tests
    ${TESTS_DIR}/deferred_raster_lighting_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_transform_gizmo_tests
    ${TESTS_DIR}/deferred_transform_gizmo_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(screenshot_capture_recorder_tests
    ${TESTS_DIR}/screenshot_capture_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_viewport_tests
    ${TESTS_DIR}/scene_viewport_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_draw_compaction_planner_tests
    ${TESTS_DIR}/bim_draw_compaction_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_frame_draw_routing_planner_tests
    ${TESTS_DIR}/bim_frame_draw_routing_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_lighting_overlay_planner_tests
    ${TESTS_DIR}/bim_lighting_overlay_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_primitive_pass_planner_tests
    ${TESTS_DIR}/bim_primitive_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_surface_draw_routing_planner_tests
    ${TESTS_DIR}/bim_surface_draw_routing_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cascade_draw_planner_tests
    ${TESTS_DIR}/shadow_cascade_draw_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(resource_pipeline_registry_tests
    ${TESTS_DIR}/resource_pipeline_registry_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(resource_pipeline_registry_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(scene_provider_extraction_tests
    ${TESTS_DIR}/scene_provider_extraction_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_compile_definitions(scene_provider_extraction_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(technique_debug_model_tests
    ${TESTS_DIR}/technique_debug_model_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(technique_debug_model_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(bim_section_cap_tests
    ${TESTS_DIR}/bim_section_cap_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_section_cap_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimSectionCapBuilder.cpp
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

add_custom_test(renderer_telemetry_tests
    ${TESTS_DIR}/renderer_telemetry_tests.cpp  ""  ${TEST_RESULTS_DIR}
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
