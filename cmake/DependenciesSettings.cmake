# cmake/DependenciesSettings.cmake

set(CMAKE_PREFIX_PATH "F:/Projects/vcpkg/installed/x64-windows;${CMAKE_PREFIX_PATH}")

# GLFW 
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# GLM 
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

