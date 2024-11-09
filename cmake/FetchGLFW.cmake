include(FetchContent)

FetchContent_Declare(
    GLFW
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4 
)
FetchContent_MakeAvailable(GLFW)
