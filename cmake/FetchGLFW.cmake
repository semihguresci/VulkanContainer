include(FetchContent)

FetchContent_Declare(
    GLFW
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4 
    SOURCE_DIR ${EXTERNAL_DIR}/glfw-src
    BINARY_DIR ${EXTERNAL_DIR}/glfw-build
)
FetchContent_MakeAvailable(GLFW)
