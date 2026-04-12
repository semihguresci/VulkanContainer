# cmake/DependenciesSettings.cmake

# vcpkg toolchain sets CMAKE_PREFIX_PATH automatically via the toolchain file
# specified in CMakePresets.json.  No hardcoded paths needed here.

# GLFW 
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# GLM 
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

