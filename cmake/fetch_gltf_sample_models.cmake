if(NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DESTINATION must be defined when fetching glTF Sample Models.")
endif()

if(NOT DEFINED STAMP_FILE)
    message(FATAL_ERROR "STAMP_FILE must be defined when fetching glTF Sample Models.")
endif()

# Skip work when a previous fetch already produced both the destination directory
# and the stamp file. This prevents re-downloading the archive on every build.
if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
    message(STATUS "glTF Sample Models already present at ${DESTINATION}; skipping download.")
    return()
endif()

set(REPO_ARCHIVE_URL "https://github.com/KhronosGroup/glTF-Sample-Models/archive/refs/heads/master.zip")
set(TEMP_DIR "${DESTINATION}.tmp")
set(ARCHIVE_PATH "${TEMP_DIR}/glTF-Sample-Models.zip")

file(REMOVE_RECURSE "${TEMP_DIR}")
file(MAKE_DIRECTORY "${TEMP_DIR}")

file(DOWNLOAD
    "${REPO_ARCHIVE_URL}"
    "${ARCHIVE_PATH}"
    STATUS download_status
    LOG download_log
    SHOW_PROGRESS
)

list(GET download_status 0 download_status_code)
if(NOT download_status_code EQUAL 0)
    list(GET download_status 1 download_status_message)
    message(FATAL_ERROR "Failed to download glTF Sample Models archive: ${download_status_message}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${ARCHIVE_PATH}"
    WORKING_DIRECTORY "${TEMP_DIR}"
    RESULT_VARIABLE extract_result
)

if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "Failed to extract glTF Sample Models archive downloaded to ${ARCHIVE_PATH}.")
endif()

file(GLOB extracted_dirs LIST_DIRECTORIES true "${TEMP_DIR}/glTF-Sample-Models-*")

if(NOT extracted_dirs)
    message(FATAL_ERROR "No extracted glTF Sample Models directory found in ${TEMP_DIR}.")
endif()

list(GET extracted_dirs 0 extracted_dir)

if(EXISTS "${DESTINATION}")
    file(REMOVE_RECURSE "${DESTINATION}")
endif()

file(RENAME "${extracted_dir}" "${DESTINATION}")

file(REMOVE_RECURSE "${TEMP_DIR}")

file(WRITE "${STAMP_FILE}" "downloaded from ${REPO_ARCHIVE_URL}\n")
