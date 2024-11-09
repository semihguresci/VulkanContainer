enable_testing()

include(cmake/FetchGoogleTest.cmake)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(TEST_RESULTS_DIR "${CMAKE_SOURCE_DIR}/test_results")
file(MAKE_DIRECTORY ${TEST_RESULTS_DIR})

set(DEFAULT_SHADER_DIR "${CMAKE_SOURCE_DIR}/tests/shaders")

function(add_custom_test TARGET_NAME SOURCE_FILE SHADER_DIR)
    add_executable(${TARGET_NAME} ${SOURCE_FILE})
    target_link_libraries(${TARGET_NAME} PRIVATE VulkanDependencies gtest gtest_main)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/tests
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<CONFIG:Release>:-O3>
    )
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME} --gtest_output=xml:${TEST_RESULTS_DIR}/${TARGET_NAME}.xml)
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${SHADER_DIR}
                $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders
            COMMENT "Copying shaders from ${SHADER_DIR} to output directory for ${TARGET_NAME}"
        )
    add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${SHADER_DIR}
            $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders
        COMMENT "Copying shaders from ${SHADER_DIR} to output directory for ${TARGET_NAME}"
    )
endfunction()

file(GLOB TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/*.cpp")

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE) 
    add_custom_test(${TEST_NAME} ${TEST_SOURCE} ${DEFAULT_SHADER_DIR})
endforeach()

set(TEST_TARGETS "")
foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    list(APPEND TEST_TARGETS ${TEST_NAME})
endforeach()

add_custom_target(build_tests
    DEPENDS ${TEST_TARGETS}
    COMMENT "Building all test executables..."
)

add_custom_target(run_tests ALL
    DEPENDS build_tests
    COMMAND ctest --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building and running all tests..."
)
