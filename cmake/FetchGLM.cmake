include(FetchContent)

FetchContent_Declare(
    GLM
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(GLM)
