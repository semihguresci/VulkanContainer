if(NOT DEFINED DESTINATION)
    message(FATAL_ERROR "DESTINATION must be defined when fetching glTF Sample Models.")
endif()

if(NOT DEFINED STAMP_FILE)
    message(FATAL_ERROR "STAMP_FILE must be defined when fetching glTF Sample Models.")
endif()

if(NOT DEFINED GIT_EXECUTABLE)
    message(FATAL_ERROR "GIT_EXECUTABLE must be defined when fetching glTF Sample Models.")
endif()

set(REPO_URL "https://github.com/KhronosGroup/glTF-Sample-Models")

set(IS_GIT_REPO FALSE)
if(EXISTS "${DESTINATION}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${DESTINATION}" rev-parse --is-inside-work-tree
        RESULT_VARIABLE repo_check
        OUTPUT_VARIABLE repo_flag
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(repo_check EQUAL 0 AND repo_flag STREQUAL "true")
        set(IS_GIT_REPO TRUE)
    endif()
endif()

if(IS_GIT_REPO)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${DESTINATION}" config --get remote.origin.url
        OUTPUT_VARIABLE remote_url
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE remote_result
    )
    if(NOT remote_result EQUAL 0)
        message(FATAL_ERROR "Unable to read remote for ${DESTINATION}.")
    endif()

    if(NOT remote_url STREQUAL "${REPO_URL}")
        message(FATAL_ERROR "Existing repository at ${DESTINATION} points to ${remote_url} (expected ${REPO_URL}).")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${DESTINATION}" pull --ff-only
        RESULT_VARIABLE pull_result
    )

    if(NOT pull_result EQUAL 0)
        message(WARNING "Failed to update glTF Sample Models repository at ${DESTINATION}; proceeding with existing checkout.")
    endif()
else()
    if(EXISTS "${DESTINATION}")
        file(REMOVE_RECURSE "${DESTINATION}")
    endif()

    file(MAKE_DIRECTORY "${DESTINATION}")

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --depth 1 "${REPO_URL}" "${DESTINATION}"
        RESULT_VARIABLE clone_result
    )

    if(NOT clone_result EQUAL 0)
        message(FATAL_ERROR "Failed to clone glTF Sample Models repository into ${DESTINATION}.")
    endif()
endif()

file(WRITE "${STAMP_FILE}" "fetched from ${REPO_URL}\n")
