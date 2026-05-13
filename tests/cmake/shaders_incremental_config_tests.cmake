if(NOT DEFINED PROJECT_SOURCE_DIR_FOR_TEST OR PROJECT_SOURCE_DIR_FOR_TEST STREQUAL "")
    message(FATAL_ERROR "PROJECT_SOURCE_DIR_FOR_TEST must point at the repository root.")
endif()

if(NOT DEFINED TEST_WORK_DIR OR TEST_WORK_DIR STREQUAL "")
    set(TEST_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders_incremental_config_tests")
endif()

set(shader_cmake "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/Shaders.cmake")
if(NOT EXISTS "${shader_cmake}")
    message(FATAL_ERROR "Expected shader script at ${shader_cmake}.")
endif()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY
    "${TEST_WORK_DIR}/fixture/shaders"
    "${TEST_WORK_DIR}/fake-vulkan/Bin")

file(TO_CMAKE_PATH "${shader_cmake}" shader_cmake_for_include)
file(WRITE "${TEST_WORK_DIR}/fixture/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.23)\n"
    "project(ShaderFixture LANGUAGES NONE)\n"
    "include(\"${shader_cmake_for_include}\")\n")

file(WRITE "${TEST_WORK_DIR}/fixture/shaders/gbuffer.slang"
    "void vertMain() {}\n"
    "void fragMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/surface_normals.slang"
    "void vertMain() {}\n"
    "void geomMain() {}\n"
    "void fragMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/tile_light_cull.slang"
    "void computeMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/brdf_common.slang"
    "float fixtureCommon() { return 1.0; }\n")

set(invocation_log "${TEST_WORK_DIR}/slangc_invocations.txt")

if(WIN32)
    set(fake_slangc "${TEST_WORK_DIR}/fake-vulkan/Bin/slangc.cmd")
    file(WRITE "${fake_slangc}"
        "@echo off\r\n"
        "setlocal enabledelayedexpansion\r\n"
        "set \"out=\"\r\n"
        "if not \"%FAKE_SLANGC_LOG%\"==\"\" echo %*>>\"%FAKE_SLANGC_LOG%\"\r\n"
        ":parse_args\r\n"
        "if \"%~1\"==\"\" goto done_parse\r\n"
        "if not \"%~1\"==\"-o\" goto next_arg\r\n"
        "shift\r\n"
        "set \"out=%~1\"\r\n"
        ":next_arg\r\n"
        "shift\r\n"
        "goto parse_args\r\n"
        ":done_parse\r\n"
        "if \"!out!\"==\"\" exit /b 2\r\n"
        "for %%I in (\"!out!\") do if not exist \"%%~dpI\" mkdir \"%%~dpI\"\r\n"
        "set \"FAKE_SLANGC_OUTPUT=!out!\"\r\n"
        "powershell -NoProfile -ExecutionPolicy Bypass -Command \"[System.IO.File]::WriteAllBytes($env:FAKE_SLANGC_OUTPUT, [byte[]](0x03, 0x02, 0x23, 0x07))\"\r\n"
        "exit /b %ERRORLEVEL%\r\n")
else()
    set(fake_slangc "${TEST_WORK_DIR}/fake-vulkan/Bin/slangc")
    file(WRITE "${fake_slangc}"
        "#!/bin/sh\n"
        "if [ -n \"$FAKE_SLANGC_LOG\" ]; then printf '%s\\n' \"$*\" >> \"$FAKE_SLANGC_LOG\"; fi\n"
        "out=''\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = '-o' ]; then shift; out=\"$1\"; fi\n"
        "  shift\n"
        "done\n"
        "if [ -z \"$out\" ]; then exit 2; fi\n"
        "mkdir -p \"$(dirname \"$out\")\"\n"
        "printf '\\003\\002#\\007' > \"$out\"\n")
    file(CHMOD "${fake_slangc}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endif()

function(count_slang_invocations output_var)
    if(EXISTS "${invocation_log}")
        file(STRINGS "${invocation_log}" invocations)
        list(LENGTH invocations count)
    else()
        set(count 0)
    endif()
    set(${output_var} "${count}" PARENT_SCOPE)
endfunction()

set(ENV{VULKAN_SDK} "${TEST_WORK_DIR}/fake-vulkan")
set(ENV{FAKE_SLANGC_LOG} "${invocation_log}")

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${TEST_WORK_DIR}/fixture"
    -B "${TEST_WORK_DIR}/build"
    "-DSLANGC_EXECUTABLE:FILEPATH=${fake_slangc}")
if(DEFINED TEST_CMAKE_GENERATOR AND NOT TEST_CMAKE_GENERATOR STREQUAL "")
    list(APPEND configure_command -G "${TEST_CMAKE_GENERATOR}")
endif()
if(DEFINED TEST_CMAKE_MAKE_PROGRAM AND NOT TEST_CMAKE_MAKE_PROGRAM STREQUAL "")
    list(APPEND configure_command "-DCMAKE_MAKE_PROGRAM:FILEPATH=${TEST_CMAKE_MAKE_PROGRAM}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Expected shader fixture project to configure.\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE first_build_result
    OUTPUT_VARIABLE first_build_stdout
    ERROR_VARIABLE first_build_stderr
)
if(NOT first_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected first shader build to succeed.\n"
        "stdout:\n${first_build_stdout}\n"
        "stderr:\n${first_build_stderr}")
endif()

set(spv_dir "${TEST_WORK_DIR}/build/spv_shaders")
foreach(expected_output IN ITEMS
        gbuffer.vert.spv
        gbuffer.frag.spv
        surface_normals.vert.spv
        surface_normals.geom.spv
        surface_normals.frag.spv
        tile_light_cull.comp.spv)
    if(NOT EXISTS "${spv_dir}/${expected_output}")
        message(FATAL_ERROR "Expected shader output ${expected_output} to be generated.")
    endif()
endforeach()

if(EXISTS "${spv_dir}/brdf_common.vert.spv" OR EXISTS "${spv_dir}/brdf_common.frag.spv")
    message(FATAL_ERROR "Include-only shader brdf_common.slang was compiled as a render shader.")
endif()

count_slang_invocations(first_invocation_count)
if(first_invocation_count EQUAL 0)
    message(FATAL_ERROR "Expected first shader build to invoke fake slangc.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE second_build_result
    OUTPUT_VARIABLE second_build_stdout
    ERROR_VARIABLE second_build_stderr
)
if(NOT second_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected second shader build to succeed.\n"
        "stdout:\n${second_build_stdout}\n"
        "stderr:\n${second_build_stderr}")
endif()

count_slang_invocations(second_invocation_count)
if(NOT second_invocation_count EQUAL first_invocation_count)
    message(FATAL_ERROR
        "Expected unchanged shader target to be incremental, but fake slangc "
        "invocations changed from ${first_invocation_count} to "
        "${second_invocation_count}.")
endif()

file(APPEND "${TEST_WORK_DIR}/fixture/shaders/gbuffer.slang"
    "float changedFixtureValue() { return 2.0; }\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE changed_build_result
    OUTPUT_VARIABLE changed_build_stdout
    ERROR_VARIABLE changed_build_stderr
)
if(NOT changed_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected changed shader build to succeed.\n"
        "stdout:\n${changed_build_stdout}\n"
        "stderr:\n${changed_build_stderr}")
endif()

count_slang_invocations(changed_invocation_count)
math(EXPR changed_invocation_delta
    "${changed_invocation_count} - ${second_invocation_count}")
if(NOT changed_invocation_delta EQUAL 2)
    message(FATAL_ERROR
        "Expected changing gbuffer.slang to rebuild only vert/frag outputs, "
        "but fake slangc invocation delta was ${changed_invocation_delta}.")
endif()
