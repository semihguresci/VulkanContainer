enable_testing()
find_package(GTest CONFIG REQUIRED)

set(TESTS_DIR "${CMAKE_SOURCE_DIR}/tests/")
set(TEST_RESULTS_DIR "${CMAKE_SOURCE_DIR}/test_results")
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

add_custom_test(rendering_convention_tests
    ${TESTS_DIR}/rendering_convention_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanContainer_geometry
)

add_custom_test(renderer_struct_tests
    ${TESTS_DIR}/renderer_struct_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanContainer_renderer
)

add_custom_test(ecs_tests
    ${TESTS_DIR}/ecs_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanContainer_ecs  VulkanContainer_scene
)

add_custom_test(scene_graph_tests
    ${TESTS_DIR}/scene_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanContainer_scene
)

# ── Tests that need Vulkan / windowing runtime ───────────────────────────────

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