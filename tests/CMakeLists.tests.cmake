enable_testing()
find_package(GTest CONFIG REQUIRED)

set(TESTS_DIR "${CMAKE_SOURCE_DIR}/tests/")
set(TEST_RESULTS_DIR "${CMAKE_SOURCE_DIR}/test_results")
file(MAKE_DIRECTORY ${TEST_RESULTS_DIR})

set(DEFAULT_SHADER_DIR "${CMAKE_SOURCE_DIR}/tests/shaders")

function(add_custom_test TARGET_NAME SOURCE_FILE SHADER_DIR TEST_RESULTS_DIR)
    # Add the executable
    add_executable(${TARGET_NAME} ${SOURCE_FILE})

    # Link the necessary libraries
    target_link_libraries(${TARGET_NAME} PRIVATE 
        VulkanDependencies 
        GTest::gtest 
        GTest::gtest_main 
        GTest::gmock 
        GTest::gmock_main
    )

    # Set target properties
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/tests
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # Set compile options for Release configuration
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<CONFIG:Release>:-O3>
    )

    # Add the test
    add_test(
        NAME ${TARGET_NAME}
        COMMAND ${TARGET_NAME} --gtest_output=xml:${TEST_RESULTS_DIR}/${TARGET_NAME}.xml
    )

    # Copy shaders to the output directory
    add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${SHADER_DIR}
            $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders
        COMMENT "Copying shaders from ${SHADER_DIR} to output directory for ${TARGET_NAME}"
    )
endfunction()
#message(FATAL_ERROR "TESTS_DIR ${TESTS_DIR}")
add_custom_test(
    glm_tests
    ${TESTS_DIR}/glm_tests.cpp
    ${DEFAULT_SHADER_DIR}
    ${TEST_RESULTS_DIR}
)

add_custom_test(
    triangle_test
    ${TESTS_DIR}/triangle_test.cpp
    ${DEFAULT_SHADER_DIR}
    ${TEST_RESULTS_DIR}
)

add_custom_test(
    vulkan_tests
    ${TESTS_DIR}/vulkan_tests.cpp
    ${DEFAULT_SHADER_DIR}
    ${TEST_RESULTS_DIR}
)

add_custom_test(
    window_creation_test
    ${TESTS_DIR}/window_creation_test.cpp
    ${DEFAULT_SHADER_DIR}
    ${TEST_RESULTS_DIR}
)