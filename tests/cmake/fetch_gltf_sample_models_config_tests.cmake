if(NOT DEFINED PROJECT_SOURCE_DIR_FOR_TEST OR PROJECT_SOURCE_DIR_FOR_TEST STREQUAL "")
    message(FATAL_ERROR "PROJECT_SOURCE_DIR_FOR_TEST must point at the repository root.")
endif()

if(NOT DEFINED TEST_WORK_DIR OR TEST_WORK_DIR STREQUAL "")
    set(TEST_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/fetch_gltf_sample_models_config_tests")
endif()

set(fetch_script "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/fetch_gltf_sample_models.cmake")
if(NOT EXISTS "${fetch_script}")
    message(FATAL_ERROR "Expected fetch script at ${fetch_script}.")
endif()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/archive_root/glTF-Sample-Models-fixture/2.0/Triangle/glTF")
file(WRITE
    "${TEST_WORK_DIR}/archive_root/glTF-Sample-Models-fixture/2.0/Triangle/glTF/Triangle.gltf"
    "{\"asset\":{\"version\":\"2.0\"}}\n")

set(archive_path "${TEST_WORK_DIR}/fixture.zip")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive_path}" --format=zip "glTF-Sample-Models-fixture"
    WORKING_DIRECTORY "${TEST_WORK_DIR}/archive_root"
    RESULT_VARIABLE create_archive_result
)
if(NOT create_archive_result EQUAL 0)
    message(FATAL_ERROR "Failed to create glTF sample model fixture archive.")
endif()

file(TO_CMAKE_PATH "${archive_path}" archive_url_path)
set(archive_url "file:///${archive_url_path}")

set(invalid_destination "${TEST_WORK_DIR}/invalid-timeout-output")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DDESTINATION:PATH=${invalid_destination}"
            "-DSTAMP_FILE:FILEPATH=${invalid_destination}/.fetched"
            "-DREPO_ARCHIVE_URL:STRING=${archive_url}"
            "-DGLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS:STRING=0"
            -P "${fetch_script}"
    RESULT_VARIABLE invalid_timeout_result
    OUTPUT_VARIABLE invalid_timeout_stdout
    ERROR_VARIABLE invalid_timeout_stderr
)
if(invalid_timeout_result EQUAL 0)
    message(FATAL_ERROR
        "Expected GLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS=0 to be rejected, "
        "but fetch_gltf_sample_models.cmake accepted it.")
endif()

set(valid_destination "${TEST_WORK_DIR}/valid-timeout-output")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DDESTINATION:PATH=${valid_destination}"
            "-DSTAMP_FILE:FILEPATH=${valid_destination}/.fetched"
            "-DREPO_ARCHIVE_URL:STRING=${archive_url}"
            "-DGLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS:STRING=30"
            "-DGLTF_SAMPLE_MODELS_INACTIVITY_TIMEOUT_SECONDS:STRING=10"
            -P "${fetch_script}"
    RESULT_VARIABLE valid_timeout_result
    OUTPUT_VARIABLE valid_timeout_stdout
    ERROR_VARIABLE valid_timeout_stderr
)
if(NOT valid_timeout_result EQUAL 0)
    message(FATAL_ERROR
        "Expected valid timeout settings to fetch the local fixture.\n"
        "stdout:\n${valid_timeout_stdout}\n"
        "stderr:\n${valid_timeout_stderr}")
endif()

if(NOT EXISTS "${valid_destination}/2.0/Triangle/glTF/Triangle.gltf")
    message(FATAL_ERROR "Fixture archive was not extracted into ${valid_destination}.")
endif()

if(NOT EXISTS "${valid_destination}/.fetched")
    message(FATAL_ERROR "Fetch script did not write the stamp file.")
endif()
