if(NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DESTINATION must be defined when fetching buildingSMART IFC5 files.")
endif()

if(NOT DEFINED STAMP_FILE)
    message(FATAL_ERROR "STAMP_FILE must be defined when fetching buildingSMART IFC5 files.")
endif()

if(NOT DEFINED REPO_REF_URL)
    set(REPO_REF_URL "https://api.github.com/repos/buildingSMART/IFC5-development/git/ref/heads/main")
endif()

set(REPO_NAME "IFC5-development")
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
        message(WARNING "Failed to resolve latest buildingSMART IFC5-development revision; using cached files at ${DESTINATION}: ${ref_status_message}")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to resolve latest buildingSMART IFC5-development revision: ${ref_status_message}")
endif()

file(READ "${REF_PATH}" ref_json)
string(REGEX REPLACE ".*\"sha\"[ \t\r\n]*:[ \t\r\n]*\"([0-9a-fA-F]+)\".*" "\\1"
       latest_sha "${ref_json}")
if(latest_sha STREQUAL ref_json OR NOT latest_sha MATCHES "^[0-9a-fA-F]+$")
    if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
        message(WARNING "Failed to parse latest buildingSMART IFC5-development revision; using cached files at ${DESTINATION}.")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to parse latest buildingSMART IFC5-development revision from ${REPO_REF_URL}.")
endif()
string(TOLOWER "${latest_sha}" latest_sha)

if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
    file(READ "${STAMP_FILE}" existing_stamp)
    if(existing_stamp MATCHES "${latest_sha}")
        message(STATUS "buildingSMART IFC5-development already present at ${DESTINATION} (${latest_sha}); skipping download.")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
endif()

set(REPO_ARCHIVE_URL
    "https://github.com/buildingSMART/IFC5-development/archive/${latest_sha}.zip")

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
        message(WARNING "Failed to refresh buildingSMART IFC5-development archive; keeping cached files at ${DESTINATION}: ${download_status_message}")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to download buildingSMART IFC5-development archive: ${download_status_message}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${ARCHIVE_PATH}"
    WORKING_DIRECTORY "${TEMP_DIR}"
    RESULT_VARIABLE extract_result
)

if(NOT extract_result EQUAL 0)
    if(EXISTS "${STAMP_FILE}" AND EXISTS "${DESTINATION}")
        message(WARNING "Failed to extract refreshed buildingSMART IFC5-development archive; keeping cached files at ${DESTINATION}.")
        file(REMOVE_RECURSE "${TEMP_DIR}")
        return()
    endif()
    message(FATAL_ERROR "Failed to extract buildingSMART IFC5-development archive downloaded to ${ARCHIVE_PATH}.")
endif()

file(GLOB extracted_dirs LIST_DIRECTORIES true "${TEMP_DIR}/IFC5-development-*")

if(NOT extracted_dirs)
    message(FATAL_ERROR "No extracted buildingSMART IFC5-development directory found in ${TEMP_DIR}.")
endif()

list(GET extracted_dirs 0 extracted_dir)

if(EXISTS "${DESTINATION}")
    file(REMOVE_RECURSE "${DESTINATION}")
endif()

file(RENAME "${extracted_dir}" "${DESTINATION}")

file(REMOVE_RECURSE "${TEMP_DIR}")
file(WRITE "${STAMP_FILE}" "downloaded commit ${latest_sha}\nsource ${REPO_ARCHIVE_URL}\n")
