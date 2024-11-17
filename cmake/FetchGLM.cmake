include(FetchContent)

FetchContent_Declare(
    GLM
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    SOURCE_DIR ${EXTERNAL_DIR}/glm-src
    BINARY_DIR ${EXTERNAL_DIR}/glm-build
)
FetchContent_MakeAvailable(GLM)
