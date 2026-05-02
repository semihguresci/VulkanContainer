if(NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DESTINATION must be defined when fetching buildingSMART Sample-Test-Files.")
endif()

if(NOT DEFINED STAMP_FILE)
    message(FATAL_ERROR "STAMP_FILE must be defined when fetching buildingSMART Sample-Test-Files.")
endif()

if(NOT DEFINED REPO_REF_URL)
    set(REPO_REF_URL "https://api.github.com/repos/buildingSMART/Sample-Test-Files/git/ref/heads/main")
endif()

set(REPO_NAME "Sample-Test-Files")
set(TEMP_DIR "${DESTINATION}.tmp")
set(REF_PATH "${TEMP_DIR}/buildingSMART-${REPO_NAME}-ref.json")
set(ARCHIVE_PATH "${TEMP_DIR}/buildingSMART-${REPO_NAME}.zip")

file(MAKE_DIRECTORY "${TEMP_DIR}")

file(DOWNLOAD
    "${REPO_REF_URL}"
    "${REF_PATH}"
    STATUS ref_status
    LOG ref_log
    TLS_VERIFY ON
    TIMEOUT 60
    HTTPHEADER "User-Agent: VulkanSceneRenderer-CMake"
)

list(GET ref_status 0 ref_status_code)
if(NOT ref_status_code EQUAL 0)
    list(GET ref_status 1 ref_status_message)
    if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
        message(WARNING "Failed to resolve latest buildingSMART Sample-Test-Files revision; using cached files at ${DESTINATION}: ${ref_status_message}")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to resolve latest buildingSMART Sample-Test-Files revision: ${ref_status_message}")
endif()

file(READ "${REF_PATH}" ref_json)
string(REGEX REPLACE ".*\"sha\"[ \t\r\n]*:[ \t\r\n]*\"([0-9a-fA-F]+)\".*" "\\1"
       latest_sha "${ref_json}")
if(latest_sha STREQUAL ref_json OR NOT latest_sha MATCHES "^[0-9a-fA-F]+$")
    if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
        message(WARNING "Failed to parse latest buildingSMART Sample-Test-Files revision; using cached files at ${DESTINATION}.")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to parse latest buildingSMART Sample-Test-Files revision from ${REPO_REF_URL}.")
endif()
string(TOLOWER "${latest_sha}" latest_sha)

if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
    file(READ "${STAMP_FILE}" existing_stamp)
    if(existing_stamp MATCHES "${latest_sha}")
        message(STATUS "buildingSMART Sample-Test-Files already present at ${DESTINATION} (${latest_sha}); skipping download.")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
endif()

set(REPO_ARCHIVE_URL
    "https://github.com/buildingSMART/Sample-Test-Files/archive/${latest_sha}.zip")

file(REMOVE_RECURSE "${TEMP_DIR}")
file(MAKE_DIRECTORY "${TEMP_DIR}")

file(DOWNLOAD
    "${REPO_ARCHIVE_URL}"
    "${ARCHIVE_PATH}"
    STATUS download_status
    LOG download_log
    TLS_VERIFY ON
    TIMEOUT 180
    SHOW_PROGRESS
    HTTPHEADER "User-Agent: VulkanSceneRenderer-CMake"
)

list(GET download_status 0 download_status_code)
if(NOT download_status_code EQUAL 0)
    list(GET download_status 1 download_status_message)
    if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
        message(WARNING "Failed to refresh buildingSMART Sample-Test-Files archive; keeping cached files at ${DESTINATION}: ${download_status_message}")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to download buildingSMART Sample-Test-Files archive: ${download_status_message}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${ARCHIVE_PATH}"
    WORKING_DIRECTORY "${TEMP_DIR}"
    RESULT_VARIABLE extract_result
)

if(NOT extract_result EQUAL 0)
    if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
        message(WARNING "Failed to extract refreshed buildingSMART Sample-Test-Files archive; keeping cached files at ${DESTINATION}.")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to extract buildingSMART Sample-Test-Files archive downloaded to ${ARCHIVE_PATH}.")
endif()

file(GLOB extracted_dirs LIST_DIRECTORIES true "${TEMP_DIR}/Sample-Test-Files-*")

if(NOT extracted_dirs)
    message(FATAL_ERROR "No extracted buildingSMART Sample-Test-Files directory found in ${TEMP_DIR}.")
endif()

list(GET extracted_dirs 0 extracted_dir)

if(EXISTS "${DESTINATION}")
    file(REMOVE_RECURSE "${DESTINATION}")
endif()

file(RENAME "${extracted_dir}" "${DESTINATION}")

file(REMOVE_RECURSE "${TEMP_DIR}")
file(WRITE "${STAMP_FILE}" "downloaded commit ${latest_sha}\nsource ${REPO_ARCHIVE_URL}\n")
