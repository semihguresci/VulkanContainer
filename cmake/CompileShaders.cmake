# cmake/CompileShaders.cmake

# Define the path to the shader compilation script
set(SHADER_COMPILE_SCRIPT "${CMAKE_SOURCE_DIR}/compile_shaders.py")

# Find Python
find_package(Python3 REQUIRED)

# Create a custom target to run the shader compilation script
add_custom_target(CompileShaders ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Running shader compilation script..."
    COMMAND ${Python3_EXECUTABLE} ${SHADER_COMPILE_SCRIPT} # Run the Python script with Python3
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} # Set the working directory to the project root
    COMMENT "Compiling shaders to SPIR-V format (.spv files)"
)