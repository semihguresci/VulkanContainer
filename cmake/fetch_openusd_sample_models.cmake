if(NOT DEFINED DESTINATION OR DESTINATION STREQUAL "")
  message(FATAL_ERROR "DESTINATION is required")
endif()
if(NOT DEFINED STAMP_FILE OR STAMP_FILE STREQUAL "")
  message(FATAL_ERROR "STAMP_FILE is required")
endif()
if(NOT DEFINED KITCHEN_SET_URL OR KITCHEN_SET_URL STREQUAL "")
  message(FATAL_ERROR "KITCHEN_SET_URL is required")
endif()
if(NOT DEFINED MEDCITY_URL OR MEDCITY_URL STREQUAL "")
  message(FATAL_ERROR "MEDCITY_URL is required")
endif()

if(EXISTS "${STAMP_FILE}" AND
   EXISTS "${DESTINATION}/Kitchen_set" AND
   EXISTS "${DESTINATION}/PointInstancedMedCity")
  message(STATUS "OpenUSD samples already present at ${DESTINATION}; skipping download.")
  return()
endif()

function(download_openusd_sample SAMPLE_NAME SAMPLE_URL)
  set(sample_dir "${DESTINATION}/${SAMPLE_NAME}")
  set(work_dir "${DESTINATION}/.${SAMPLE_NAME}.tmp")
  set(archive "${work_dir}/${SAMPLE_NAME}.zip")
  set(extract_dir "${work_dir}/extract")

  message(STATUS "Downloading OpenUSD sample ${SAMPLE_NAME}")
  file(REMOVE_RECURSE "${work_dir}")
  file(MAKE_DIRECTORY "${work_dir}")
  file(DOWNLOAD
       "${SAMPLE_URL}"
       "${archive}"
       STATUS download_status
       SHOW_PROGRESS)
  list(GET download_status 0 download_code)
  list(GET download_status 1 download_message)
  if(NOT download_code EQUAL 0)
    if(EXISTS "${sample_dir}")
      message(WARNING
        "Failed to refresh OpenUSD sample ${SAMPLE_NAME}: ${download_message}. "
        "Keeping existing files in ${sample_dir}.")
      file(REMOVE_RECURSE "${work_dir}")
      return()
    endif()
    message(FATAL_ERROR
      "Failed to download OpenUSD sample ${SAMPLE_NAME}: ${download_message}")
  endif()

  file(REMOVE_RECURSE "${extract_dir}")
  file(MAKE_DIRECTORY "${extract_dir}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
    WORKING_DIRECTORY "${extract_dir}"
    RESULT_VARIABLE extract_result)
  if(NOT extract_result EQUAL 0)
    if(EXISTS "${sample_dir}")
      message(WARNING
        "Failed to extract OpenUSD sample ${SAMPLE_NAME}. Keeping existing files.")
      file(REMOVE_RECURSE "${work_dir}")
      return()
    endif()
    message(FATAL_ERROR "Failed to extract OpenUSD sample ${SAMPLE_NAME}")
  endif()

  if(NOT EXISTS "${extract_dir}/${SAMPLE_NAME}")
    message(FATAL_ERROR
      "OpenUSD sample ${SAMPLE_NAME} did not contain expected root directory")
  endif()

  file(REMOVE_RECURSE "${sample_dir}")
  file(RENAME "${extract_dir}/${SAMPLE_NAME}" "${sample_dir}")
  file(REMOVE_RECURSE "${work_dir}")
endfunction()

file(MAKE_DIRECTORY "${DESTINATION}")
download_openusd_sample("Kitchen_set" "${KITCHEN_SET_URL}")
download_openusd_sample("PointInstancedMedCity" "${MEDCITY_URL}")

file(WRITE "${STAMP_FILE}"
  "downloaded OpenUSD samples\n"
  "Kitchen_set ${KITCHEN_SET_URL}\n"
  "PointInstancedMedCity ${MEDCITY_URL}\n")
