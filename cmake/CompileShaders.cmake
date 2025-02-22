# Set the path to your Python script
set(SHADER_COMPILER_SCRIPT "${CMAKE_SOURCE_DIR}/compile_shaders.py")

find_package(Python3 REQUIRED)

# Set the build directory where shaders will be compiled
set(SHADER_BUILD_DIR "${CMAKE_BINARY_DIR}/shaders")

# Create the output directory (optional, but recommended)
file(MAKE_DIRECTORY "${SHADER_BUILD_DIR}")

# Execute the Python script
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_SOURCE_DIR}" python3 "${SHADER_COMPILER_SCRIPT}" "${SHADER_BUILD_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE SHADER_COMPILER_RESULT
    OUTPUT_VARIABLE SHADER_COMPILER_OUTPUT
    ERROR_VARIABLE SHADER_COMPILER_ERROR
)

# Check the result of the script execution
if (NOT SHADER_COMPILER_RESULT EQUAL 0)
    message(FATAL_ERROR "Shader compilation failed:\n${SHADER_COMPILER_OUTPUT}\n${SHADER_COMPILER_ERROR}")
else()
    message(STATUS "Shader compilation successful:\n${SHADER_COMPILER_OUTPUT}")
endif()